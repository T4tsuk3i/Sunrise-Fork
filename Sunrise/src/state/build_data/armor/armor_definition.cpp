#include "armor_definition.h"

namespace sunrise::state::build_data::armor {
namespace {

/** Every archetype maps primary and secondary by the authored Armor 3.0 design. */
constexpr ArchetypeTable kArchetypes{
    .values{ArchetypeDefinition{Archetype::paragon, stats::Stat::super, stats::Stat::melee},
            ArchetypeDefinition{Archetype::grenadier,
                                stats::Stat::grenade,
                                stats::Stat::super},
            ArchetypeDefinition{Archetype::specialist,
                                stats::Stat::clazz,
                                stats::Stat::weapons},
            ArchetypeDefinition{Archetype::brawler, stats::Stat::melee, stats::Stat::health},
            ArchetypeDefinition{Archetype::bulwark, stats::Stat::health, stats::Stat::clazz},
            ArchetypeDefinition{Archetype::gunner, stats::Stat::weapons, stats::Stat::grenade}},
};

/** The launch-model tier budgets: each tier a slightly larger stat band. */
constexpr TierTable kTiers{
    .values{TierBudget{GearTier::one, 52, 57},
            TierBudget{GearTier::two, 58, 63},
            TierBudget{GearTier::three, 64, 69},
            TierBudget{GearTier::four, 70, 75},
            TierBudget{GearTier::five, 75, 75}},
};

} // namespace

const ArchetypeDefinition* find_archetype(Archetype archetype) noexcept {
    const std::size_t index = static_cast<std::size_t>(archetype);
    if (index >= kArchetypes.values.size() || kArchetypes.values[index].archetype != archetype) {
        return nullptr;
    }
    return &kArchetypes.values[index];
}

const TierBudget* find_tier(GearTier tier) noexcept {
    const std::size_t index = static_cast<std::size_t>(tier);
    if (index >= kTiers.values.size() || kTiers.values[index].tier != tier) {
        return nullptr;
    }
    return &kTiers.values[index];
}

std::int32_t masterwork_bonus(std::int32_t level) noexcept {
    if (level <= 0) {
        return 0;
    }
    return level > kMasterworkMaxLevel ? kMasterworkMaxLevel : level;
}

} // namespace sunrise::state::build_data::armor