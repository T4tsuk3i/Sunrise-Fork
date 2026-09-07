#pragma once

#include <cstdint>

#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/abilities/definition.h"
#include "../../../../state/build_data/items/details/definition.h"
#include "../../family4/loadout/definition.h"
#include "../layout.h"

namespace sunrise::middleware::datagen::character_record::appearance {

namespace details = state::build_data::items::details;

/** First equipment slot a weapon occupies; the three weapons own the per-weapon tables. */
inline constexpr std::int8_t kFirstWeaponSlot = 7;
/** Equipment slots a weapon can occupy, one per per-weapon table. */
inline constexpr std::size_t kWeaponSlotCount = 3;
/** The subclass occupies the first equipment slot and owns the ability buckets. */
inline constexpr std::uint8_t kSubclassEquipmentSlot = 0;

/** One equipped item, resolved to the installed detail and the plugs its sockets hold. */
struct Equipped {
    std::uint8_t equipmentSlot{};
    std::uint16_t definitionIndex{details::kUnavailableItemIndex};
    /** Effective plug per socket lane, unavailable where the lane holds nothing. */
    std::array<std::uint16_t, details::kInitialPlugCapacity> plugs{};
    std::size_t laneCount{};
};

/** Selects class-qualified art when present, otherwise the definition's generic arrangement. */
[[nodiscard]] constexpr std::uint16_t
select_art_arrangement(const details::Definition& detail,
                       state::CharacterClass characterClass) noexcept {
    const std::size_t classSlot = static_cast<std::size_t>(characterClass) + 1U;
    if (classSlot < detail.artArrangementIndices.size()
        && detail.artArrangementIndices[classSlot] != details::kUnavailableArtIndex) {
        return detail.artArrangementIndices[classSlot];
    }
    return detail.artArrangementIndices.front();
}

/**
 * Resolves one equipped instance to its detail and the plugs its sockets hold.
 * The resolver has already applied the authored or native-default socket policy, so a lane that
 * reads unavailable holds nothing rather than needing a second fallback here.
 * @param slotted Resolved instance and its native equipment slot.
 * @param detail Receives the base item's installed detail.
 * @param equipped Receives the slot, index and effective plug lanes.
 * @return True when the base item has an installed detail.
 */
[[nodiscard]] bool resolve_equipped(const family4::loadout::SlottedInstance& slotted,
                                    details::Definition& detail,
                                    Equipped& equipped) noexcept;

/** Fills every empty-valued field with the sentinel its reader tests for. */
void apply_sentinels(layout::Appearance& appearance) noexcept;

/**
 * Fills each equipped render row with its instance, definition, art and material pairs.
 * @param instances Resolved item instances belonging to one character.
 * @param characterClass Class whose art rows are selected when a definition carries them.
 * @param appearance Appearance block receiving the render rows.
 * @return True when every instance addresses a render row.
 */
[[nodiscard]] bool apply_render(const family4::loadout::ResolvedInstances& instances,
                                state::CharacterClass characterClass,
                                layout::Appearance& appearance) noexcept;

/**
 * Fills the 12 ability buckets from the character's subclass and ability picks.
 * @param character Validated authored character.
 * @param instances Resolved item instances belonging to that character.
 * @param appearance Appearance block receiving the buckets.
 * @return True when the subclass resolves and its buckets are published.
 */
[[nodiscard]] bool apply_ability_buckets(const state::CharacterState& character,
                                         const family4::loadout::ResolvedInstances& instances,
                                         layout::Appearance& appearance) noexcept;

/**
 * Fills the overflow hash bank from every equipped socket plug, in socket-type priority order.
 * Runs after the ability buckets, which own whatever overflow slots the subclass claims, and takes
 * only the slots still holding the no-hash sentinel.
 * @param instances Resolved item instances belonging to one character.
 * @param appearance Appearance block receiving the bank.
 */
void apply_overflow_hashes(const family4::loadout::ResolvedInstances& instances,
                           layout::Appearance& appearance) noexcept;

/**
 * Fills the character-wide and the three per-weapon sandbox perk banks.
 * @param instances Resolved item instances belonging to one character.
 * @param appearance Appearance block receiving the banks.
 */
void apply_perk_banks(const family4::loadout::ResolvedInstances& instances,
                      layout::Appearance& appearance) noexcept;

/**
 * Reports the completed appearance block's ability buckets, overflow bank and perk-bank fills once.
 * Runs after every fill, so this is the exact wire content the client's ability and perk path
 * reads, independent of what the build data declared. When the perk census is enabled it also
 * names every sandbox perk the equipped set contributed and the bank entries that survived, which
 * is what separates "the record never carried the perk" from "the client ignored it".
 * @param soid Character the record belongs to, so a three-character encode names its subject.
 * @param instances Resolved equipped set the banks were filled from, for perk attribution.
 * @param appearance Completed appearance block, all fills applied.
 */
void report_encoded_probe(std::uint64_t soid,
                          const family4::loadout::ResolvedInstances& instances,
                          const layout::Appearance& appearance) noexcept;

/**
 * Fills the character stat table and the three per-weapon stat tables.
 * @param character Character being encoded, which identifies the diagnostics' subject.
 * @param instances Resolved item instances belonging to one character.
 * @param light Equipment light, which goes in the character table's light row.
 * @param appearance Appearance block receiving the tables.
 * @return True when the installed investment constants name the rows to write.
 */
[[nodiscard]] bool apply_stats(const state::CharacterState& character,
                               const family4::loadout::ResolvedInstances& instances,
                               std::int32_t light,
                               layout::Appearance& appearance) noexcept;

} // namespace sunrise::middleware::datagen::character_record::appearance
