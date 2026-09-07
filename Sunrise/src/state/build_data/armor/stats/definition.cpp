#include "definition.h"

namespace sunrise::state::build_data::armor::stats {
namespace {

/**
 * The authored stat table. Every value is an initial tuning guess and belongs to this one place
 * so the sandbox can be rebalanced by editing a single table rather than scattered gameplay code.
 * Legacy names map 1:1 to their modern equivalents; numerical values convert at the same count.
 */
constexpr Table kTable{
    .values{Definition{Stat::weapons,
                       "Weapons",
                       "Mobility",
                       0,
                       101,
                       Effect{.basePerPoint = 0.15F,
                              .basePerPointPvP = 0.15F,
                              .enhancedPerPoint = 10.0F,
                              .enhancedPerPointPvP = 10.0F},
                       false},
             Definition{Stat::health,
                        "Health",
                        "Resilience",
                        1,
                        101,
                        Effect{.basePerPoint = 0.1F,
                               .basePerPointPvP = 0.1F,
                               .enhancedPerPoint = 1.0F,
                               .enhancedPerPointPvP = 1.0F},
                        false},
             Definition{Stat::clazz,
                        "Class",
                        "Recovery",
                        2,
                        101,
                        Effect{.basePerPoint = 1.0F,
                               .basePerPointPvP = 1.0F,
                               .enhancedPerPoint = 1.0F,
                               .enhancedPerPointPvP = 1.0F},
                        true},
             Definition{Stat::grenade,
                        "Grenade",
                        "Discipline",
                        3,
                        101,
                        Effect{.basePerPoint = 1.0F,
                               .basePerPointPvP = 1.0F,
                               .enhancedPerPoint = 0.65F,
                               .enhancedPerPointPvP = 0.2F},
                        false},
             Definition{Stat::super,
                        "Super",
                        "Intellect",
                        4,
                        101,
                        Effect{.basePerPoint = 1.0F,
                               .basePerPointPvP = 1.0F,
                               .enhancedPerPoint = 0.45F,
                               .enhancedPerPointPvP = 0.45F},
                        false},
             Definition{Stat::melee,
                        "Melee",
                        "Strength",
                        5,
                        101,
                        Effect{.basePerPoint = 1.0F,
                               .basePerPointPvP = 1.0F,
                               .enhancedPerPoint = 0.3F,
                               .enhancedPerPointPvP = 0.2F},
                        false}},
};

} // namespace

const Table& table() noexcept {
    return kTable;
}

const Definition* find(Stat stat) noexcept {
    const std::size_t index = static_cast<std::size_t>(stat);
    if (index >= kTable.values.size() || kTable.values[index].stat != stat) {
        return nullptr;
    }
    return &kTable.values[index];
}

} // namespace sunrise::state::build_data::armor::stats
