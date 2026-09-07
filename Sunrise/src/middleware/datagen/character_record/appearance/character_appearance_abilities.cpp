#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::middleware::datagen::character_record::appearance {
namespace {

namespace buckets = state::build_data::abilities;

/** The authored equipment slot that holds the subclass. */
constexpr std::size_t kSubclassSlot =
    static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);

/** @param item Authored subclass item. @return Its 5 selected socket entries. */
[[nodiscard]] buckets::Selection
selection_of(const state::account::inventory::Item& item) noexcept {
    return {item.movementAbilityEntry,
            item.grenadeAbilityEntry,
            item.superAbilityEntry,
            item.meleeAbilityEntry,
            item.classAbilityEntry};
}

/** Diagnostic latch: one character's encode settles the ability probe for a run. */
std::atomic<bool> g_reportedAbilityProbe{};

/**
 * Reports the subclass's declared ability buckets once.
 * Logs what the build data's ability catalog carries for the character's subclass and ability
 * selection, which is the source a mis-resolved record would diverge from. The client's cooldown
 * path reads the encoded buckets, so "the record is empty but the catalog is not" and "the catalog
 * is empty" are problems in different places.
 * @param present True when an equipped subclass instance was found.
 * @param subclassDefinition Installed definition of the equipped subclass.
 * @param socketEntryListIndex Subclass's ability socket-entry list.
 * @param selection The five selected ability entries.
 * @param resolved Whether find_ability_buckets resolved this selection.
 * @param published Catalog buckets for this (entry list, selection).
 */
void report_ability_probe(bool present,
                          std::uint16_t subclassDefinition,
                          std::uint16_t socketEntryListIndex,
                          buckets::Selection selection,
                          bool resolved,
                          const buckets::Definition& published) noexcept {
    if (g_reportedAbilityProbe.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    {
        std::array<char, 256> line{};
        const int n = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=ability stage=probe present=%d subclass=%u entry_list=%u "
                                    "sel=%u/%u/%u/%u/%u result=%s",
                                    present ? 1 : 0,
                                    static_cast<unsigned>(subclassDefinition),
                                    static_cast<unsigned>(socketEntryListIndex),
                                    static_cast<unsigned>(selection.movementEntry),
                                    static_cast<unsigned>(selection.grenadeEntry),
                                    static_cast<unsigned>(selection.superEntry),
                                    static_cast<unsigned>(selection.meleeEntry),
                                    static_cast<unsigned>(selection.classEntry),
                                    resolved ? "ok" : "fail");
        if (n > 0) {
            core::log::write(core::log::Channel::middleware,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(n)});
        }
    }
    for (std::size_t bucket = 0; bucket < published.buckets.size(); ++bucket) {
        const buckets::Bucket& declared = published.buckets[bucket];
        std::array<char, 256> line{};
        const int n = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=ability stage=bucket b=%zu kind=%u count=%u "
                                    "h0=0x%08X h1=0x%08X",
                                    bucket,
                                    static_cast<unsigned>(declared.kind),
                                    static_cast<unsigned>(declared.hashCount),
                                    declared.hashes[0],
                                    declared.hashes[1]);
        if (n > 0) {
            core::log::write(core::log::Channel::middleware,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(n)});
        }
    }
    std::array<char, 256> line{};
    const int n = std::snprintf(line.data(),
                                line.size(),
                                "ev=ability stage=overflow count=%u h0=0x%08X h1=0x%08X",
                                static_cast<unsigned>(published.overflowCount),
                                published.overflow[0],
                                published.overflow[1]);
    if (n > 0) {
        core::log::write(core::log::Channel::middleware,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(n)});
    }
}

} // namespace

/** Fills the 12 ability buckets from the character's subclass and ability picks. */
bool apply_ability_buckets(const state::CharacterState& character,
                           const family4::loadout::ResolvedInstances& instances,
                           layout::Appearance& appearance) noexcept {
    // Every character is encoded at character select, so the probe follows the selected one only.
    const bool subject = character.selected;
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        if (instances.items[index].equipmentSlot != kSubclassEquipmentSlot) {
            continue;
        }
        const auto& subclassItem = character.equipment.slots[kSubclassSlot];
        if (!subclassItem.has_value()) {
            return false;
        }
        details::Definition detail{};
        buckets::Definition published{};
        if (!state::build_data::find_configured_item_detail(
                instances.items[index].instance.baseDefinitionIndex, detail)) {
            return false;
        }
        const bool resolved = state::build_data::find_ability_buckets(
            detail.socketEntryListIndex, selection_of(*subclassItem), published);
        if (subject) {
            report_ability_probe(true,
                                 instances.items[index].instance.baseDefinitionIndex,
                                 detail.socketEntryListIndex,
                                 selection_of(*subclassItem),
                                 resolved,
                                 published);
        }
        if (!resolved) {
            // The domain has not caught up with this selection yet. Publish empty buckets for
            // this encode, like a character with no subclass, instead of failing: a hard failure
            // aborts the whole Family-0/3 snapshot even though the selection did commit.
            return true;
        }
        for (std::size_t bucket = 0; bucket < appearance.abilityBuckets.size(); ++bucket) {
            layout::AbilityBucket& target = appearance.abilityBuckets[bucket];
            target.kind = static_cast<std::int8_t>(published.buckets[bucket].kind);
            for (std::size_t entry = 0; entry < published.buckets[bucket].hashCount; ++entry) {
                target.hashes[entry] = published.buckets[bucket].hashes[entry];
            }
        }
        // The overflow bank takes the subclass hashes no bucket category claims. Whatever it does
        // not fill stays at the no-hash sentinel for the equipped plug hashes to take.
        for (std::size_t entry = 0; entry < published.overflowCount; ++entry) {
            appearance.overflowHashes[entry] = published.overflow[entry];
        }
        return true;
    }
    // A character with no subclass equipped publishes empty buckets, which is what the client
    // computes for it as well. The probe still fires so an empty record is distinguishable from a
    // subclass whose selection simply never resolved.
    if (subject) {
        report_ability_probe(false,
                             layout::kEmptyDefinitionIndex,
                             layout::kEmptyDefinitionIndex,
                             {},
                             false,
                             {});
    }
    return true;
}

} // namespace sunrise::middleware::datagen::character_record::appearance
