/**
 * Searches the live process for the character stat block the sandbox reads.
 *
 * VMProtect leaves both code sections at full entropy on disk, so nothing can be learned from the
 * file. Inside the process the same bytes are plaintext, and Sunrise is already inside it. What
 * makes the search tractable is that the answer is known on one side: the six values encoded into
 * the character record are ours, and the client honours exactly three of them.
 */

#include "stat_block_scan.h"

#include <windows.h>

#include <psapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"

namespace sunrise::client::diagnostics {
namespace {

/** Values one scan looks for, matching the record's six character stat rows. */
constexpr std::size_t kValueCapacity = core::settings::client::kStatScanValueCapacity;
/** Hits reported before the scan stops logging, so a common value cannot flood the file. */
constexpr std::size_t kHitLimit = 200;
/** Bytes read at a time, so no single read reserves much. */
constexpr std::size_t kChunkBytes = 1U << 20U;
/** Words logged either side of a hit. */
constexpr std::size_t kContextValues = 8;
/** Highest user-mode address walked. */
constexpr std::uintptr_t kUserAddressLimit = 0x7FFFFFFFFFFFULL;
/** Stack swept either side of a hit when collecting the addresses that built the frame. */
constexpr std::size_t kStackSweepBytes = 1024;
/** Distinct code addresses reported per hit. */
constexpr std::size_t kCodeAddressLimit = 16;
/** Bytes dumped at each code address. */
constexpr std::size_t kCodeDumpBytes = 96;
/** Bytes of the dump that precede the address, so the call that reached the frame is inside it. */
constexpr std::size_t kCodeLeadBytes = 48;

/** @param protection Region protection. @return True when the region can be read. */
[[nodiscard]] bool readable(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ
                                | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (protection & kReadable) != 0;
}

/** Writes one prepared line. */
void emit(std::string_view line) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::info, line);
}

/** One scan request, copied once so settings are read on the calling thread only. */
struct Request {
    std::array<std::int32_t, kValueCapacity> values{};
    std::size_t valueCount{};
    std::size_t windowBytes{};
    std::uint32_t delayMilliseconds{};
    /** Gap before the next pass, or zero to stop after the first one. */
    std::uint32_t intervalMilliseconds{};
};

/** Bytes this module occupies, so the scan stops reporting its own settings copy as a find. */
struct SelfRange {
    std::uintptr_t base{};
    std::size_t size{};
};

/** @return This module's loaded range, or an empty range when it cannot be resolved. */
[[nodiscard]] SelfRange self_range() noexcept {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&emit),
                           &module)
            == 0
        || module == nullptr) {
        return {};
    }
    MODULEINFO info{};
    if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof info) == 0) {
        return {};
    }
    return {reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage};
}

/** @return The executable's loaded range, which is the only place its decrypted code exists. */
[[nodiscard]] SelfRange main_module_range() noexcept {
    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return {};
    }
    MODULEINFO info{};
    if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof info) == 0) {
        return {};
    }
    return {reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage};
}

/** Reinterprets one authored integer as the float the same number would be stored as. */
[[nodiscard]] std::int32_t as_float_bits(std::int32_t value) noexcept {
    const auto scalar = static_cast<float>(value);
    std::int32_t bits = 0;
    std::memcpy(&bits, &scalar, sizeof bits);
    return bits;
}

/**
 * Checks whether every remaining value appears in order after the first match.
 * The block stride is unknown, so a window is searched rather than a fixed layout: a match means
 * the values sit close together in order, which is what a stat array of any stride looks like.
 * @param chunk Bytes read from the region.
 * @param start Offset of the first value match.
 * @param request Values being searched for.
 * @param offsets Receives each value offset from the first match.
 * @return True when all values matched inside the window.
 */
[[nodiscard]] bool match_window(std::span<const std::byte> chunk,
                                std::size_t start,
                                const Request& request,
                                std::array<std::size_t, kValueCapacity>& offsets) noexcept {
    offsets[0] = 0;
    std::size_t cursor = start + sizeof(std::int32_t);
    const std::size_t reach = start + request.windowBytes;
    const std::size_t limit = reach < chunk.size() ? reach : chunk.size();
    for (std::size_t index = 1; index < request.valueCount; ++index) {
        bool found = false;
        for (std::size_t probe = cursor; probe + sizeof(std::int32_t) <= limit;
             probe += sizeof(std::int32_t)) {
            std::int32_t value = 0;
            std::memcpy(&value, chunk.data() + probe, sizeof value);
            if (value == request.values[index]) {
                offsets[index] = probe - start;
                cursor = probe + sizeof(std::int32_t);
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

/** Logs one hit with its address, region and the offsets the values landed at. */
void report_hit(const std::byte* address,
                const MEMORY_BASIC_INFORMATION& region,
                const Request& request,
                const std::array<std::size_t, kValueCapacity>& offsets) noexcept {
    std::array<char, 256> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=stat_scan stage=hit at=0x%llX base=0x%llX size=0x%llX "
                                      "prot=0x%lX off=",
                                      reinterpret_cast<unsigned long long>(address),
                                      reinterpret_cast<unsigned long long>(region.BaseAddress),
                                      static_cast<unsigned long long>(region.RegionSize),
                                      static_cast<unsigned long>(region.Protect));
    if (written <= 0) {
        return;
    }
    auto used = static_cast<std::size_t>(written);
    for (std::size_t index = 0; index < request.valueCount && used + 8 < line.size(); ++index) {
        const int extra = std::snprintf(
            line.data() + used, line.size() - used, "%s%zu", index == 0 ? "" : ",", offsets[index]);
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
    emit({line.data(), used});
}

/** Logs the words either side of a hit, which is what names the block stride and its neighbours. */
void report_context(const std::byte* address) noexcept {
    std::array<std::int32_t, kContextValues * 2> window{};
    const std::byte* start = address - kContextValues * sizeof(std::int32_t);
    MEMORY_BASIC_INFORMATION probe{};
    if (VirtualQuery(start, &probe, sizeof probe) == 0 || !readable(probe.Protect)) {
        return;
    }
    std::memcpy(window.data(), start, sizeof window);
    std::array<char, 256> line{};
    const int written = std::snprintf(line.data(), line.size(), "ev=stat_scan stage=context v=");
    if (written <= 0) {
        return;
    }
    auto used = static_cast<std::size_t>(written);
    for (std::size_t index = 0; index < window.size() && used + 14 < line.size(); ++index) {
        const int extra = std::snprintf(
            line.data() + used, line.size() - used, "%s%d", index == 0 ? "" : ",", window[index]);
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
    emit({line.data(), used});
}

/**
 * Dumps the bytes at one code address so it can be disassembled off-process, and reports whether
 * the byte at the address itself is a bare unconditional jump.
 * The image is encrypted on disk and plaintext only here, so the bytes have to travel out through
 * the log. A return address points past its call, so the dump starts before it and the instruction
 * that reached the frame sits inside the window rather than off its front edge. A candidate that
 * turns out to be a bare `jmp` rather than a call site is most likely a linker thunk or an
 * obfuscator's redirect stub, and the destination it names is where the real logic sits.
 * @param address Code address recovered from the stack.
 * @param image The executable's loaded range, used to report a stable offset.
 * @param label Distinguishes this dump's origin in the log line.
 * @param target Receives the jump's absolute destination when the address holds one.
 * @return True when a short or near jump sits at the address and named a followable target.
 */
[[nodiscard]] bool report_code_bytes(std::uintptr_t address,
                                     const SelfRange& image,
                                     const char* label,
                                     std::uintptr_t& target) noexcept {
    target = 0;
    const std::uintptr_t start = address - kCodeLeadBytes;
    MEMORY_BASIC_INFORMATION probe{};
    if (VirtualQuery(reinterpret_cast<const void*>(start), &probe, sizeof probe) == 0
        || !readable(probe.Protect)) {
        return false;
    }
    std::array<std::byte, kCodeDumpBytes> bytes{};
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(start), bytes.size());
    std::array<char, 320> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=stat_scan stage=code label=%s rva=0x%llX lead=%zu b=",
                                      label,
                                      static_cast<unsigned long long>(start - image.base),
                                      kCodeLeadBytes);
    if (written <= 0) {
        return false;
    }
    auto used = static_cast<std::size_t>(written);
    for (std::size_t index = 0; index < bytes.size() && used + 3 < line.size(); ++index) {
        const int extra = std::snprintf(line.data() + used,
                                        line.size() - used,
                                        "%02X",
                                        static_cast<unsigned>(std::to_integer<unsigned char>(
                                            bytes[index])));
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
    emit({line.data(), used});
    // The address itself sits kCodeLeadBytes into this window. EB is a short jump (1-byte signed
    // displacement); E9 is a near jump (4-byte signed displacement). Anything else is not a bare
    // jump and there is no target to follow.
    const auto opcode = std::to_integer<unsigned char>(bytes[kCodeLeadBytes]);
    if (opcode == 0xEBU && kCodeLeadBytes + 1 < bytes.size()) {
        const auto displacement =
            static_cast<std::int8_t>(std::to_integer<unsigned char>(bytes[kCodeLeadBytes + 1]));
        target = address + 2 + static_cast<std::intptr_t>(displacement);
        return true;
    }
    if (opcode == 0xE9U && kCodeLeadBytes + 4 < bytes.size()) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, &bytes[kCodeLeadBytes + 1], sizeof displacement);
        target = address + 5 + static_cast<std::intptr_t>(displacement);
        return true;
    }
    return false;
}

/**
 * Reports the executable addresses surrounding a hit, which is what makes a stack hit worth having.
 * A frame that has already returned still holds the addresses of the calls that built it, and each
 * one points into decrypted text. Those are the callers that had this character's stats in hand.
 * @param address Address the values matched at.
 * @param image The executable's loaded range; addresses outside it are ignored.
 */
void report_stack_callers(const std::byte* address, const SelfRange& image) noexcept {
    if (image.size == 0) {
        return;
    }
    std::array<std::uintptr_t, kCodeAddressLimit> seen{};
    std::size_t count = 0;
    const auto hit = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t first = hit > kStackSweepBytes ? hit - kStackSweepBytes : 0;
    const std::uintptr_t last = hit + kStackSweepBytes;
    for (std::uintptr_t cursor = (first + 7U) & ~std::uintptr_t{7U};
         cursor + sizeof(std::uintptr_t) <= last && count < seen.size();
         cursor += sizeof(std::uintptr_t)) {
        MEMORY_BASIC_INFORMATION probe{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &probe, sizeof probe) == 0
            || !readable(probe.Protect)) {
            continue;
        }
        std::uintptr_t candidate = 0;
        std::memcpy(&candidate, reinterpret_cast<const void*>(cursor), sizeof candidate);
        if (candidate < image.base || candidate >= image.base + image.size) {
            continue;
        }
        bool known = false;
        for (std::size_t index = 0; index < count; ++index) {
            known = known || seen[index] == candidate;
        }
        if (known) {
            continue;
        }
        seen[count] = candidate;
        ++count;
        std::array<char, 200> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=stat_scan stage=caller rva=0x%llX at=0x%llX slot=%lld",
                                          static_cast<unsigned long long>(candidate - image.base),
                                          static_cast<unsigned long long>(candidate),
                                          static_cast<long long>(cursor)
                                              - static_cast<long long>(hit));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        std::uintptr_t target = 0;
        if (report_code_bytes(candidate, image, "stack", target)) {
            // A bare jump at the candidate itself is not the reading code; it is a stub pointing
            // at it. One hop is followed, not a chain, so an obfuscator's redirect ladder cannot
            // turn this into an unbounded walk.
            std::uintptr_t unused = 0;
            (void)report_code_bytes(target, image, "jump_target", unused);
        }
    }
}

/** Walks every committed readable region and reports each window that carries the values. */
void scan(const Request& request,
          const SelfRange& self,
          const SelfRange& image,
          const char* form,
          std::size_t pass) noexcept {
    static std::array<std::byte, kChunkBytes> chunk{};
    std::size_t hits = 0;
    std::size_t regions = 0;
    std::uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (address < kUserAddressLimit
           && VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof region) != 0) {
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const std::size_t size = region.RegionSize;
        if (size == 0) {
            break;
        }
        const bool owned = self.size != 0 && base >= self.base && base < self.base + self.size;
        if (region.State == MEM_COMMIT && readable(region.Protect) && !owned) {
            ++regions;
            for (std::size_t offset = 0; offset < size && hits < kHitLimit; offset += kChunkBytes) {
                const std::size_t left = size - offset;
                const std::size_t take = left < kChunkBytes ? left : kChunkBytes;
                std::memcpy(chunk.data(), reinterpret_cast<const void*>(base + offset), take);
                const std::span<const std::byte> view(chunk.data(), take);
                for (std::size_t probe = 0; probe + sizeof(std::int32_t) <= take && hits < kHitLimit;
                     probe += sizeof(std::int32_t)) {
                    std::int32_t value = 0;
                    std::memcpy(&value, view.data() + probe, sizeof value);
                    if (value != request.values[0]) {
                        continue;
                    }
                    std::array<std::size_t, kValueCapacity> offsets{};
                    if (!match_window(view, probe, request, offsets)) {
                        continue;
                    }
                    const auto* hit = reinterpret_cast<const std::byte*>(base + offset + probe);
                    report_hit(hit, region, request, offsets);
                    report_context(hit);
                    report_stack_callers(hit, image);
                    ++hits;
                }
            }
        }
        if (base + size <= address) {
            break;
        }
        address = base + size;
    }
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=stat_scan stage=done pass=%zu form=%s regions=%zu "
                                      "hits=%zu limit=%zu",
                                      pass,
                                      form,
                                      regions,
                                      hits,
                                      kHitLimit);
    if (written > 0) {
        emit({line.data(), static_cast<std::size_t>(written)});
    }
}

Request g_request{};

/**
 * Runs one scan pass guarded, so a region that changes protection mid-read cannot take the process.
 * @param pass Ordinal of this pass, stamped on its log lines so a repeating scan's hits can be
 * told apart by which pass found them.
 */
void guarded_scan(std::size_t pass) noexcept {
    const SelfRange self = self_range();
    const SelfRange image = main_module_range();
    std::array<char, 160> banner{};
    const int stamped = std::snprintf(banner.data(),
                                      banner.size(),
                                      "ev=stat_scan stage=image pass=%zu base=0x%llX size=0x%llX",
                                      pass,
                                      static_cast<unsigned long long>(image.base),
                                      static_cast<unsigned long long>(image.size));
    if (stamped > 0) {
        emit({banner.data(), static_cast<std::size_t>(stamped)});
    }
    __try {
        scan(g_request, self, image, "int", pass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit("ev=stat_scan stage=done form=int result=fault");
    }
    // A sandbox that keeps stats as scalars stores the same numbers as floats, and the integer
    // pass cannot see those at all, so the same query runs again over their bit patterns.
    Request scalar = g_request;
    for (std::size_t index = 0; index < scalar.valueCount; ++index) {
        scalar.values[index] = as_float_bits(scalar.values[index]);
    }
    __try {
        scan(scalar, self, image, "float", pass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit("ev=stat_scan stage=done form=float result=fault");
    }
}

/**
 * Sleeps out the configured delay, then runs scan passes until the interval is zero or the pass
 * cap is reached.
 * The block a stat sits in is a stable snapshot, but the code that reads it only touches the stack
 * for the instant an ability computes something from it. A single pass has no way to land on that
 * instant; repeating the pass across ordinary play does, without requiring the delay to be timed
 * against a button press. Zero interval keeps the original one-shot behaviour.
 */
DWORD WINAPI scan_thread(LPVOID) noexcept {
    Sleep(g_request.delayMilliseconds);
    for (std::size_t pass = 0; pass < core::settings::client::kMaximumStatScanPasses; ++pass) {
        std::array<char, 192> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=stat_scan stage=begin pass=%zu values=%zu window=%zu "
                                          "delay_ms=%u interval_ms=%u",
                                          pass,
                                          g_request.valueCount,
                                          g_request.windowBytes,
                                          g_request.delayMilliseconds,
                                          g_request.intervalMilliseconds);
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        guarded_scan(pass);
        if (g_request.intervalMilliseconds == 0) {
            break;
        }
        Sleep(g_request.intervalMilliseconds);
    }
    return 0;
}

} // namespace

/** Schedules the stat block search when the settings name values to look for. */
bool start_stat_block_scan() noexcept {
    const core::settings::client::Settings& settings = core::settings::get().client;
    // Whether the scan runs at all is decided by having values to search for. A zero delay is a
    // real request to start at hook activation, not a way to turn the scan off.
    if (settings.statScanValueCount == 0) {
        return false;
    }
    g_request = {};
    g_request.valueCount = settings.statScanValueCount < kValueCapacity ? settings.statScanValueCount
                                                                       : kValueCapacity;
    for (std::size_t index = 0; index < g_request.valueCount; ++index) {
        g_request.values[index] = settings.statScanValues[index];
    }
    g_request.windowBytes = settings.statScanWindowBytes;
    g_request.delayMilliseconds = static_cast<std::uint32_t>(settings.statScanDelayMs);
    g_request.intervalMilliseconds = static_cast<std::uint32_t>(settings.statScanIntervalMs);
    const HANDLE thread = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        emit("ev=stat_scan stage=begin result=thread_fail");
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace sunrise::client::diagnostics
