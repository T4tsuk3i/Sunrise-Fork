/**
 * Searches live process memory for internal build cvar/flag name literals.
 *
 * The console is disabled on this build, and nothing in Sunrise names a damage or invulnerability
 * flag, so there is no code path to trace from. What an unreleased build usually still has is the
 * flag's own registration name as a short ASCII string literal, compiled into read-only data
 * plaintext in memory even though the file is encrypted on disk. This walks committed readable
 * memory for a fixed list of likely candidate names, the same one-shot VEH-guarded technique
 * `stat_block_scan` uses for its int/float search, adapted to bytes instead of numbers.
 */

#include "debug_flag_scan.h"

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

/**
 * Candidate internal build flag/cvar names, guessed from common conventions across engines with a
 * console (id Tech, Source) since this build's own naming scheme is undocumented. Deliberately
 * excludes bare dictionary words ("god", "cheats", "immortal") that dialogue and item text would
 * match constantly; every entry here is a compound token that plain narrative text would not
 * produce by chance.
 */
constexpr std::array<std::string_view, 14> kCandidates{{
    "notarget",
    "noclip",
    "buddha",
    "godmode",
    "god_mode",
    "sv_cheats",
    "damagescale",
    "damage_scale",
    "playerdamage",
    "player_damage",
    "nodamage",
    "no_damage",
    "cheatsenabled",
    "infinitehealth",
}};

/** Hits reported before the scan stops logging, so a common substring cannot flood the file. */
constexpr std::size_t kHitLimit = 150;
/** Hits reported for any one candidate, so a single noisy match cannot spend the whole budget. */
constexpr std::size_t kPerCandidateLimit = 25;
/**
 * The one candidate whose full literal is worth chasing to whatever references it. A hit on this
 * word is only trusted as the real flag name when this exact prefix sits right before it in the
 * same chunk, which is what tells a genuine "player_no_damage" literal apart from an unrelated
 * "no_damage" substring elsewhere.
 */
constexpr std::string_view kBackreferenceWord = "no_damage";
constexpr std::string_view kBackreferencePrefix = "player_";
/** Confirmed string addresses worth searching for as a raw pointer value. */
constexpr std::size_t kBackreferenceTargetCapacity = 4;
/** Backreference hits reported before that pass stops logging. */
constexpr std::size_t kBackreferenceHitLimit = 60;
/** Bytes of raw hex printed either side of a backreference hit. */
constexpr std::size_t kBackreferenceContextBytes = 64;
/** Bytes read at a time, so no single read reserves much. */
constexpr std::size_t kChunkBytes = 1U << 20U;
/** Context printed either side of a hit; string literals are usually pooled by the compiler, so a
 *  real hit's neighbours are worth reading by eye for names not on the candidate list. */
constexpr std::size_t kContextRadius = 420;
/** Highest user-mode address walked. */
constexpr std::uintptr_t kUserAddressLimit = 0x7FFFFFFFFFFFULL;

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

/** Copies bytes as printable text, substituting '.' for anything outside the printable ASCII range. */
void printable_copy(std::span<char> dest, std::span<const std::byte> source) noexcept {
    const std::size_t count = (std::min)(dest.size(), source.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<unsigned char>(source[index]);
        dest[index] = (value >= 0x20U && value < 0x7FU) ? static_cast<char>(value) : '.';
    }
}

/** Logs one hit with its address, the matched candidate and the surrounding bytes as text. */
void report_hit(std::string_view candidate,
                const std::byte* address,
                const MEMORY_BASIC_INFORMATION& region,
                std::span<const std::byte> regionView,
                std::size_t offsetInRegion) noexcept {
    std::array<char, kContextRadius * 2 + 4> contextText{};
    const std::size_t radius = (std::min)(kContextRadius, contextText.size() / 2);
    const std::size_t start = offsetInRegion > radius ? offsetInRegion - radius : 0;
    const std::size_t end = (std::min)(offsetInRegion + radius, regionView.size());
    printable_copy(std::span(contextText).first(end - start), regionView.subspan(start, end - start));

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=debug_flag_scan stage=hit word=%.*s at=0x%llX base=0x%llX "
                                      "prot=0x%lX ctx=%.*s",
                                      static_cast<int>(candidate.size()),
                                      candidate.data(),
                                      reinterpret_cast<unsigned long long>(address),
                                      reinterpret_cast<unsigned long long>(region.BaseAddress),
                                      static_cast<unsigned long>(region.Protect),
                                      static_cast<int>(end - start),
                                      contextText.data());
    if (written > 0) {
        emit({line.data(), static_cast<std::size_t>(written)});
    }
}

/** Logs one place in memory that holds a raw pointer to a confirmed flag-name string. */
void report_backreference_hit(std::uintptr_t target,
                              const std::byte* address,
                              const MEMORY_BASIC_INFORMATION& region,
                              std::span<const std::byte> window) noexcept {
    std::array<char, 96> head{};
    const int headWritten = std::snprintf(head.data(),
                                          head.size(),
                                          "ev=debug_flag_scan stage=backref target=0x%llX at=0x%llX "
                                          "base=0x%llX prot=0x%lX b=",
                                          static_cast<unsigned long long>(target),
                                          reinterpret_cast<unsigned long long>(address),
                                          reinterpret_cast<unsigned long long>(region.BaseAddress),
                                          static_cast<unsigned long>(region.Protect));
    if (headWritten <= 0) {
        return;
    }
    std::array<char, 320> line{};
    auto used = static_cast<std::size_t>(
        (std::min)(headWritten, static_cast<int>(line.size())));
    std::memcpy(line.data(), head.data(), used);
    for (const std::byte byte : window) {
        if (used + 3 >= line.size()) {
            break;
        }
        const int extra = std::snprintf(line.data() + used,
                                        line.size() - used,
                                        "%02X",
                                        static_cast<unsigned>(std::to_integer<unsigned char>(byte)));
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
    emit({line.data(), used});
}

/**
 * Walks every committed readable region again, this time for a raw pointer match instead of a
 * string. Whatever holds one of these addresses referenced the flag-name literal by pointer,
 * which is what a debug-menu or cvar descriptor entry looks like next to its backing value.
 * @param targets Confirmed string addresses from this run's candidate scan.
 * @param self This module's own loaded range, excluded the same way the string pass excludes it.
 */
void scan_for_pointers(std::span<const std::uintptr_t> targets, const ModuleRange& self) noexcept {
    if (targets.empty()) {
        return;
    }
    static std::array<std::byte, kChunkBytes> chunk{};
    std::size_t hits = 0;
    std::uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (address < kUserAddressLimit
           && VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof region) != 0) {
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const std::size_t size = region.RegionSize;
        if (size == 0) {
            break;
        }
        const bool owned = contains(self, base);
        if (region.State == MEM_COMMIT && readable(region.Protect) && !owned) {
            for (std::size_t offset = 0; offset < size && hits < kBackreferenceHitLimit;
                 offset += kChunkBytes) {
                const std::size_t left = size - offset;
                const std::size_t take = left < kChunkBytes ? left : kChunkBytes;
                std::memcpy(chunk.data(), reinterpret_cast<const void*>(base + offset), take);
                for (std::size_t probe = 0;
                     probe + sizeof(std::uintptr_t) <= take && hits < kBackreferenceHitLimit;
                     probe += alignof(std::uintptr_t)) {
                    std::uintptr_t value = 0;
                    std::memcpy(&value, chunk.data() + probe, sizeof value);
                    bool matched = false;
                    for (const std::uintptr_t target : targets) {
                        matched = matched || value == target;
                    }
                    if (!matched) {
                        continue;
                    }
                    const std::size_t radius = kBackreferenceContextBytes / 2;
                    const std::size_t start = probe > radius ? probe - radius : 0;
                    const std::size_t end = (std::min)(probe + radius, take);
                    const auto* hit = reinterpret_cast<const std::byte*>(base + offset + probe);
                    report_backreference_hit(
                        value, hit, region, std::span(chunk).subspan(start, end - start));
                    ++hits;
                }
            }
        }
        if (base + size <= address) {
            break;
        }
        address = base + size;
    }
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=debug_flag_scan stage=backref_done hits=%zu limit=%zu",
                                      hits,
                                      kBackreferenceHitLimit);
    if (written > 0) {
        emit({line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Walks every committed readable region and reports each candidate name found in it.
 * @param self This module's own loaded range, excluded so its own settings copy is not a hit.
 * @param backreferenceTargets Receives confirmed `player_no_damage` string addresses this run, so
 *                             the caller can chase them with a pointer-value pass afterward.
 */
void scan(const ModuleRange& self,
         std::array<std::uintptr_t, kBackreferenceTargetCapacity>& backreferenceTargets,
         std::size_t& backreferenceTargetCount) noexcept {
    static std::array<std::byte, kChunkBytes> chunk{};
    std::array<std::size_t, kCandidates.size()> perCandidateHits{};
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
        const bool owned = contains(self, base);
        if (region.State == MEM_COMMIT && readable(region.Protect) && !owned) {
            ++regions;
            for (std::size_t offset = 0; offset < size && hits < kHitLimit; offset += kChunkBytes) {
                const std::size_t left = size - offset;
                const std::size_t take = left < kChunkBytes ? left : kChunkBytes;
                std::memcpy(chunk.data(), reinterpret_cast<const void*>(base + offset), take);
                const std::span<const std::byte> view(chunk.data(), take);
                for (std::size_t candidateIndex = 0;
                     candidateIndex < kCandidates.size() && hits < kHitLimit;
                     ++candidateIndex) {
                    if (perCandidateHits[candidateIndex] >= kPerCandidateLimit) {
                        continue;
                    }
                    const std::string_view word = kCandidates[candidateIndex];
                    if (word.size() > take) {
                        continue;
                    }
                    for (std::size_t probe = 0;
                         probe + word.size() <= take && hits < kHitLimit
                         && perCandidateHits[candidateIndex] < kPerCandidateLimit;
                         ++probe) {
                        if (std::memcmp(view.data() + probe, word.data(), word.size()) != 0) {
                            continue;
                        }
                        const auto* hit = reinterpret_cast<const std::byte*>(base + offset + probe);
                        report_hit(word, hit, region, view, probe);
                        ++hits;
                        ++perCandidateHits[candidateIndex];
                        if (word == kBackreferenceWord
                            && backreferenceTargetCount < backreferenceTargets.size()
                            && probe >= kBackreferencePrefix.size()
                            && std::memcmp(view.data() + probe - kBackreferencePrefix.size(),
                                          kBackreferencePrefix.data(),
                                          kBackreferencePrefix.size())
                                   == 0) {
                            backreferenceTargets[backreferenceTargetCount] =
                                base + offset + probe - kBackreferencePrefix.size();
                            ++backreferenceTargetCount;
                        }
                    }
                }
            }
        }
        if (base + size <= address) {
            break;
        }
        address = base + size;
    }
    std::array<char, 128> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=debug_flag_scan stage=done regions=%zu hits=%zu limit=%zu",
        regions, hits, kHitLimit);
    if (written > 0) {
        emit({line.data(), static_cast<std::size_t>(written)});
    }
}

/** Sleeps out the configured delay, then runs one scan. */
DWORD WINAPI scan_thread(LPVOID) noexcept {
    const core::settings::client::Settings& settings = core::settings::get().client;
    Sleep(static_cast<DWORD>(settings.debugFlagScanDelayMs));
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=debug_flag_scan stage=begin words=%zu delay_ms=%llu",
                                      kCandidates.size(),
                                      static_cast<unsigned long long>(settings.debugFlagScanDelayMs));
    if (written > 0) {
        emit({line.data(), static_cast<std::size_t>(written)});
    }
    const ModuleRange self = self_range();
    std::array<std::uintptr_t, kBackreferenceTargetCapacity> backreferenceTargets{};
    std::size_t backreferenceTargetCount = 0;
    __try {
        scan(self, backreferenceTargets, backreferenceTargetCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit("ev=debug_flag_scan stage=done result=fault");
        return 0;
    }
    __try {
        scan_for_pointers(std::span(backreferenceTargets).first(backreferenceTargetCount), self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit("ev=debug_flag_scan stage=backref_done result=fault");
    }
    return 0;
}

} // namespace

/** Schedules the one-shot debug-flag string search when the settings ask for one. */
bool start_debug_flag_scan() noexcept {
    if (core::settings::get().client.debugFlagScanDelayMs == 0) {
        return false;
    }
    const HANDLE thread = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        emit("ev=debug_flag_scan stage=begin result=thread_fail");
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace sunrise::client::diagnostics
