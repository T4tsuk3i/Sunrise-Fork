#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "stats/definition.h"

namespace sunrise::state::build_data::armor {

/** The six Armor 3.0 archetypes define which two stats an armor roll favours. */
enum class Archetype : std::uint8_t {
    paragon,
    grenadier,
    specialist,
    brawler,
    bulwark,
    gunner,
    count,
};

inline constexpr std::size_t kArchetypeCount = static_cast<std::size_t>(Archetype::count);

/** One archetype: its primary and secondary stat, with a pool for the tertiary pick. */
struct ArchetypeDefinition {
    Archetype archetype{};
    /** Highest-priority stat. */
    stats::Stat primary{};
    /** Second-highest-priority stat. */
    stats::Stat secondary{};
    /** Stats the tertiary roll may draw from, excluding primary and secondary. */
    std::array<stats::Stat, stats::kStatCount - 2> tertiaryPool{};
    std::size_t tertiaryPoolCount{};
};

/** All six archetypes keyed by their authored index. */
struct ArchetypeTable {
    std::array<ArchetypeDefinition, kArchetypeCount> values{};
};

/** @return The definition for one archetype, or null for an empty table. */
[[nodiscard]] const ArchetypeDefinition* find_archetype(Archetype archetype) noexcept;

/** The five gear tiers with progressively larger stat budgets. */
enum class GearTier : std::uint8_t {
    one,
    two,
    three,
    four,
    five,
    count,
};

inline constexpr std::size_t kGearTierCount = static_cast<std::size_t>(GearTier::count);
inline constexpr std::int32_t kMasterworkMaxLevel = 5;

/** Stat budget of one gear tier. */
struct TierBudget {
    GearTier tier{};
    std::int32_t minimum{};
    std::int32_t maximum{};
};

/** The authored tier-budget table, configurable without touching the roller. */
struct TierTable {
    std::array<TierBudget, kGearTierCount> values{};
};

/** @return The stat budget for one gear tier, or null for an empty table. */
[[nodiscard]] const TierBudget* find_tier(GearTier tier) noexcept;

/** @return The authored masterwork per-level bonus to each of the three lowest stats. */
[[nodiscard]] std::int32_t masterwork_bonus(std::int32_t level) noexcept;

/** One armor set and its two-piece and four-piece bonus. */
struct SetDefinition {
    std::uint32_t setHash{};
    const char* name{};
    std::uint32_t twoPieceBonus{};
    std::uint32_t fourPieceBonus{};
};

} // namespace sunrise::state::build_data::armor
