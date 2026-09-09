/**
 * Locates the character stat block once, then polls it for the rest of the session.
 *
 * `stat_block_scan` proved the block is a packed float[6] at a fixed 4-byte stride, so locating
 * it here needs no window search: the first value found is checked directly against the next
 * five at consecutive offsets. What this adds over that one-shot scan is standing watch on it --
 * a background thread reads the same six floats on an interval for as long as the process runs,
 * and logs whenever one changes, so a whole play session's worth of ability use can be checked
 * against the timeline without a second boot.
 */

#include "stat_block_watch.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "module_range.h"

namespace sunrise::client::diagnostics {
namespace {

/** The record's six character stat rows, in the authored table order. */
constexpr std::size_t kStatSlotCount = 6;
constexpr std::array<std::string_view, kStatSlotCount> kSlotNames{{
    "weapons",
    "health",
    "clazz",
    "grenade",
    "super",
    "melee",
}};

/** Bytes read at a time while locating the block, so no single read reserves much. */
constexpr std::size_t kChunkBytes = 1U << 20U;
/** Highest user-mode address walked. */
constexpr std::uintptr_t kUserAddressLimit = 0x7FFFFFFFFFFFULL;
/** Polling interval used when the setting leaves it at zero. */
constexpr std::uint64_t kDefaultIntervalMs = 250;
/** Change lines reported before watching stops logging, so a fast-ticking value cannot flood it. */
constexpr std::size_t kMaxChangeReports = 500;
/**
 * Wait between locate attempts, and attempts allowed, before giving up.
 * The block looks like it only exists once the stats screen has actually rendered at least once,
 * so a single attempt right after the delay can miss it entirely if that screen was not opened
 * yet. Retrying for several minutes covers a normal boot-then-check-your-sheet sequence without
 * costing a restart when the timing does not line up.
 */
constexpr std::uint64_t kLocateRetryIntervalMs = 2000;
constexpr std::size_t kLocateRetryLimit = 150;

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

/** @return This module's loaded range, or an empty range when it cannot be resolved. */
[[nodiscard]] ModuleRange self_range() noexcept {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&emit),
                           &module)
            == 0
        || module == nullptr) {
        return {};
    }
    ModuleRange range{};
    (void)module_range(module, range);
    return range;
}

/**
 * @return This thread's own stack range.
 * The search target lives in a local array for the whole walk, so without this the walk can find
 * that array sitting on its own stack instead of the game's memory: an instant, self-matching
 * false hit that then "changes" on every poll as later calls reuse the same stack bytes for
 * something else entirely.
 */
[[nodiscard]] ModuleRange own_stack_range() noexcept {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return {static_cast<std::uintptr_t>(low), static_cast<std::uintptr_t>(high)};
}

/** Reinterprets one authored integer as the float the same number would be stored as. */
[[nodiscard]] std::int32_t as_float_bits(std::int32_t value) noexcept {
    const auto scalar = static_cast<float>(value);
    std::int32_t bits = 0;
    std::memcpy(&bits, &scalar, sizeof bits);
    return bits;
}

/**
 * Walks every committed readable region for the six configured values, packed tightly as floats
 * in order. Stops at the first match, which is the whole block since the stride is already known.
 * @param values The six stat values to search for, as their authored integers.
 * @param self This module's own loaded range, excluded the same way the other scans exclude it.
 * @param stack This thread's own stack range, excluded so the search target's local copy can
 *              never match itself.
 * @param address Receives the block's start address only on success.
 * @return True when a run of all six values was found in order at a 4-byte stride.
 */
[[nodiscard]] bool locate_block(std::span<const std::int32_t> values,
                                const ModuleRange& self,
                                const ModuleRange& stack,
                                std::byte*& address) noexcept {
    std::array<std::int32_t, kStatSlotCount> wanted{};
    for (std::size_t index = 0; index < kStatSlotCount; ++index) {
        wanted[index] = as_float_bits(values[index]);
    }
    static std::array<std::byte, kChunkBytes> chunk{};
    const std::size_t span = kStatSlotCount * sizeof(std::int32_t);
    std::uintptr_t cursor = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (cursor < kUserAddressLimit
           && VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof region) != 0) {
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const std::size_t size = region.RegionSize;
        if (size == 0) {
            break;
        }
        const bool owned = contains(self, base) || contains(stack, base);
        if (region.State == MEM_COMMIT && readable(region.Protect) && !owned && size >= span) {
            for (std::size_t offset = 0; offset < size; offset += kChunkBytes) {
                const std::size_t left = size - offset;
                const std::size_t take = left < kChunkBytes ? left : kChunkBytes;
                if (take < span) {
                    continue;
                }
                std::memcpy(chunk.data(), reinterpret_cast<const void*>(base + offset), take);
                for (std::size_t probe = 0; probe + span <= take; probe += sizeof(std::int32_t)) {
                    bool matched = true;
                    for (std::size_t index = 0; index < kStatSlotCount && matched; ++index) {
                        std::int32_t value = 0;
                        std::memcpy(&value,
                                   chunk.data() + probe + index * sizeof(std::int32_t),
                                   sizeof value);
                        matched = value == wanted[index];
                    }
                    if (matched) {
                        address = reinterpret_cast<std::byte*>(base + offset + probe);
                        return true;
                    }
                }
            }
        }
        if (base + size <= cursor) {
            break;
        }
        cursor = base + size;
    }
    return false;
}

/** Logs one slot's value changing, with the log line's own timestamp doing the correlation work. */
void report_change(std::size_t slot, float previous, float current) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=stat_watch stage=change slot=%.*s old=%f new=%f",
                                      static_cast<int>(kSlotNames[slot].size()),
                                      kSlotNames[slot].data(),
                                      static_cast<double>(previous),
                                      static_cast<double>(current));
    if (written > 0) {
        emit({line.data(), static_cast<std::size_t>(written)});
    }
}

/** Sleeps out the configured delay, locates the block, then polls it until the process exits. */
DWORD WINAPI watch_thread(LPVOID) noexcept {
    const core::settings::client::Settings& settings = core::settings::get().client;
    Sleep(static_cast<DWORD>(settings.statWatchDelayMs));
    if (settings.statScanValueCount < kStatSlotCount) {
        emit("ev=stat_watch stage=done result=fail reason=values");
        return 0;
    }

    const ModuleRange self = self_range();
    const ModuleRange stack = own_stack_range();
    std::byte* address = nullptr;
    bool found = false;
    for (std::size_t attempt = 0; attempt < kLocateRetryLimit && !found; ++attempt) {
        if (attempt != 0) {
            Sleep(static_cast<DWORD>(kLocateRetryIntervalMs));
        }
        __try {
            found = locate_block(
                std::span(settings.statScanValues).first(kStatSlotCount), self, stack, address);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            emit("ev=stat_watch stage=done result=fault_locate");
            return 0;
        }
    }
    if (!found) {
        emit("ev=stat_watch stage=done result=not_found");
        return 0;
    }
    std::array<char, 96> found_line{};
    const int foundWritten = std::snprintf(found_line.data(),
                                           found_line.size(),
                                           "ev=stat_watch stage=found at=0x%llX",
                                           reinterpret_cast<unsigned long long>(address));
    if (foundWritten > 0) {
        emit({found_line.data(), static_cast<std::size_t>(foundWritten)});
    }

    std::array<float, kStatSlotCount> last{};
    __try {
        std::memcpy(last.data(), address, sizeof last);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit("ev=stat_watch stage=done result=fault_read");
        return 0;
    }

    const DWORD intervalMs = settings.statWatchIntervalMs != 0
                                 ? static_cast<DWORD>(settings.statWatchIntervalMs)
                                 : static_cast<DWORD>(kDefaultIntervalMs);
    std::size_t reports = 0;
    for (;;) {
        Sleep(intervalMs);
        std::array<float, kStatSlotCount> current{};
        bool ok = true;
        __try {
            std::memcpy(current.data(), address, sizeof current);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (!ok) {
            emit("ev=stat_watch stage=done result=fault_read");
            return 0;
        }
        for (std::size_t index = 0; index < kStatSlotCount; ++index) {
            if (current[index] != last[index]) {
                if (reports < kMaxChangeReports) {
                    report_change(index, last[index], current[index]);
                    ++reports;
                }
                last[index] = current[index];
            }
        }
        if (reports >= kMaxChangeReports) {
            emit("ev=stat_watch stage=done result=limit");
            return 0;
        }
    }
}

} // namespace

/** Schedules the one-shot locate-then-poll when the settings ask for one. */
bool start_stat_block_watch() noexcept {
    const core::settings::client::Settings& settings = core::settings::get().client;
    {
        std::array<char, 160> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=stat_watch stage=begin delay_ms=%llu",
                                          static_cast<unsigned long long>(settings.statWatchDelayMs));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (settings.statWatchDelayMs == 0) {
        return false;
    }
    const HANDLE thread = CreateThread(nullptr, 0, &watch_thread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        emit("ev=stat_watch stage=begin result=thread_fail");
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace sunrise::client::diagnostics
