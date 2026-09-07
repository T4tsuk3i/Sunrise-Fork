#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::armor::stats {

/** The six modern armor stats the build presents under their modern names. */
enum class Stat : std::uint8_t {
    weapons,
    health,
    clazz,
    grenade,
    super,
    melee,
    count,
};

/** Index of every supported modern stat in the authored table order. */
inline constexpr std::size_t kStatCount = static_cast<std::size_t>(Stat::count);

/** The effective modern stat never exceeds this, matching the Armor 3.0 design ceiling. */
inline constexpr std::int32_t kMaximumStat = 200;

/** Every character starts at this Class stat before any armor or mod contribution. */
inline constexpr std::int32_t kBaseClazzStat = 60;

/**
 * One stat's tuned effect. Each range (0-100 baseline, 101-200 enhanced) is independently
 * configurable for PvE and PvP so the sandbox balances without rewriting the stat system.
 */
struct Effect {
    /** Baseline 0-100 effect per point in PvE. */
    float basePerPoint{};
    /** Baseline 0-100 effect per point in PvP. */
    float basePerPointPvP{};
    /** Enhanced 101-200 effect per point in PvE. */
    float enhancedPerPoint{};
    /** Enhanced 101-200 effect per point in PvP. */
    float enhancedPerPointPvP{};
};

/** One modern stat's authored behaviour: display name, legacy alias and tuned effect. */
struct Definition {
    /** Modern stat this definition describes. */
    Stat stat{};
    /** Modern display name the sheet and diagnostics present. */
    const char* displayName{};
    /** Legacy Armor 2.0 row name this stat translates from. */
    const char* legacyName{};
    /**
     * Position of this stat's legacy row among the six rows the installed investment constants
     * name, in ascending order. The true per-stat association is confirmed by the per-row bonus
     * probe, so this value is data to adjust, not a hardcoded assumption.
     */
    std::uint8_t legacyRowIndex{};
    /** First point of the enhanced range; 0-100 is the baseline effect. */
    std::int32_t enhancedStart{101};
    /** Tuned baseline and enhanced effect, PvE and PvP separate. */
    Effect effect{};
    /** Whether a character begins with a fixed baseline before any armor is added. */
    bool baseline{false};
};

/** The six stat definitions, ordered by the authored table index. */
struct Table {
    std::array<Definition, kStatCount> values{};
};

/**
 * @return The authored definition for one modern stat, or null when the table is empty.
 */
[[nodiscard]] const Definition* find(Stat stat) noexcept;

/**
 * @return The static, always-present definition table.
 */
[[nodiscard]] const Table& table() noexcept;

} // namespace sunrise::state::build_data::armor::stats
