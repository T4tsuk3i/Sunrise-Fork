#include "loadout_item_resolver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <array>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/runtime.h"
#include "subclass_socket_selection.h"

namespace sunrise::middleware::datagen::family4::loadout {
namespace {

namespace authored_inventory = state::account::inventory;
namespace build_buckets = state::build_data::inventory::buckets;
namespace build_details = state::build_data::items::details;
namespace build_items = state::build_data::items;
namespace build_socket_lists = state::build_data::socket_entry_lists;

/** Native instanced inventory rows always carry exactly one item. */
constexpr std::int32_t kInstancedQuantity = 1;

/**
 * Finds the installed item index for one authored plug hash.
 * @param definitionHash Authored plug definition hash.
 * @param itemDefinitionCount Stable dense item-table row count.
 * @param output Receives the installed plug index.
 * @return True when the hash has one mapping inside the stable table bound.
 */
[[nodiscard]] bool resolve_plug(std::uint32_t definitionHash,
                                std::size_t itemDefinitionCount,
                                std::uint16_t& output) noexcept {
    build_items::Definition definition{};
    if (!state::build_data::find_item_definition_hash(definitionHash, definition)
        || static_cast<std::size_t>(definition.definitionIndex) >= itemDefinitionCount) {
        return false;
    }
    output = definition.definitionIndex;
    return true;
}

/**
 * Resolves native-default or authored ordinary socket lanes.
 * @param authored Authored socket policy and optional plug hashes.
 * @param definition Installed item detail carrying the native socket shape.
 * @param itemDefinitionCount Stable dense item-table row count.
 * @param output Receives the fully resolved ordinary socket block.
 * @return True when policy, lane count, and every present plug resolve.
 */
[[nodiscard]] bool resolve_ordinary_sockets(const authored_inventory::Sockets& authored,
                                            const build_details::Definition& definition,
                                            std::size_t itemDefinitionCount,
                                            instance::OrdinarySockets& output) noexcept {
    output = {};
    if (definition.ordinarySocketState == build_details::OrdinarySocketState::absent) {
        if (authored.policy == authored_inventory::SocketPolicy::authored
            && authored.plugCount != 0) {
            return false;
        }
        output.state = instance::OrdinarySocketBlockState::absent;
        return true;
    }
    if (definition.ordinarySocketState != build_details::OrdinarySocketState::present) {
        return false;
    }
    output.state = instance::OrdinarySocketBlockState::present;

    if (authored.policy == authored_inventory::SocketPolicy::nativeDefaults) {
        for (std::size_t index = 0; index < definition.initialPlugIndices.size(); ++index) {
            const std::uint16_t plugIndex = definition.initialPlugIndices[index];
            if (plugIndex == build_details::kUnavailableItemIndex) {
                continue;
            }
            if (static_cast<std::size_t>(plugIndex) >= itemDefinitionCount) {
                return false;
            }
            output.plugs[index] = plugIndex;
        }
        return true;
    }
    if (authored.policy != authored_inventory::SocketPolicy::authored
        || authored.plugCount != definition.ordinarySocketCount) {
        return false;
    }
    for (std::size_t index = 0; index < authored.plugCount; ++index) {
        const std::optional<std::uint32_t>& definitionHash = authored.plugs[index];
        if (!definitionHash.has_value()) {
            continue;
        }
        std::uint16_t plugIndex = 0;
        if (!resolve_plug(*definitionHash, itemDefinitionCount, plugIndex)) {
            return false;
        }
        output.plugs[index] = plugIndex;
    }
    return true;
}

/**
 * Applies the native instanced-or-stackable quantity policy.
 * @param authored Positive authored quantity.
 * @param definition Installed item detail carrying the stack limit and predicate.
 * @param output Receives a positive row quantity.
 * @return True when the normalized predicate is supported.
 */
[[nodiscard]] bool resolve_quantity(const authored_inventory::Item& authored,
                                    const build_details::Definition& definition,
                                    std::int32_t& output) noexcept {
    switch (definition.instancedDefinitionState) {
    case build_details::InstancedDefinitionState::stackable:
        output = (std::min)(authored.quantity, definition.maxStackSize);
        return output > 0;
    case build_details::InstancedDefinitionState::instanced:
        output = kInstancedQuantity;
        return true;
    default:
        return false;
    }
}

} // namespace

/** Resolves one authored item into native mappings without choosing an inventory row. */
bool resolve_item(const authored_inventory::Item& authored,
                  const state::CharacterState& character,
                  std::size_t itemDefinitionCount,
                  std::size_t socketEntryListCount,
                  bool requireEquipmentSlot,
                  Candidate& output) noexcept {
    if (!authored_inventory::valid(authored) || itemDefinitionCount == 0
        || itemDefinitionCount > build_items::kDefinitionCapacity || socketEntryListCount == 0
        || socketEntryListCount > build_socket_lists::kDefinitionCapacity) {
        return false;
    }

    build_items::Definition itemDefinition{};
    build_details::Definition itemDetail{};
    build_buckets::Descriptor bucket{};
    build_socket_lists::Definition socketList{};
    if (!state::build_data::find_item_definition_hash(authored.definitionHash, itemDefinition)
        || !state::build_data::find_configured_item_detail(itemDefinition.definitionIndex,
                                                           itemDetail)
        || itemDefinition.bucketId != itemDetail.bucketId
        // A pursuit - a bounty or a quest step - names no equipment slot, because nothing equips
        // it. Requiring one refused it here, so it was added to the inventory and then could not
        // be found in the resolved loadout, and the acquisition failed as `resolve_or_bucket_full`.
        // Equipped items still must name a slot: they come out of the equipment array, where the
        // slot is what identifies them.
        || (requireEquipmentSlot && !itemDetail.equipmentSlot.has_value())
        || (itemDetail.equipmentSlot.has_value() && *itemDetail.equipmentSlot < 0)
        || !state::build_data::find_inventory_bucket_descriptor(itemDetail.bucketId, bucket)
        || bucket.arraySelector != build_buckets::ArraySelector::character
        || !state::build_data::find_socket_entry_list(itemDetail.socketEntryListIndex, socketList)
        || static_cast<std::size_t>(itemDefinition.definitionIndex) >= itemDefinitionCount
        || static_cast<std::size_t>(socketList.definitionIndex) >= socketEntryListCount
        || socketList.definitionIndex != itemDetail.socketEntryListIndex) {
        return false;
    }

    Candidate candidate{};
    candidate.bucket = bucket;
    // Slot zero for a slotless item is safe: the encoder reads `equipmentSlot` only when `equipped`
    // is set, and only items resolved out of the equipment array are ever equipped.
    candidate.item.equipmentSlot =
        itemDetail.equipmentSlot.has_value()
            ? static_cast<std::uint8_t>(*itemDetail.equipmentSlot)
            : std::uint8_t{0};
    candidate.item.mutationSerial = authored.mutationSerial;
    candidate.item.flags = authored.flags;
    if (!resolve_quantity(authored, itemDetail, candidate.item.quantity)
        || !resolve_ordinary_sockets(authored.sockets,
                                     itemDetail,
                                     itemDefinitionCount,
                                     candidate.item.instance.ordinarySockets)) {
        return false;
    }

    candidate.item.instance.instanceSoid = authored.instanceSoid;
    candidate.item.instance.bounds.itemDefinitionCount =
        static_cast<std::uint32_t>(itemDefinitionCount);
    candidate.item.instance.bounds.socketEntryListCount =
        static_cast<std::uint32_t>(socketEntryListCount);
    candidate.item.instance.baseDefinitionIndex = itemDefinition.definitionIndex;
    candidate.item.instance.level = authored.level;
    candidate.item.instance.armor.archetype = authored.armorArchetype;
    candidate.item.instance.armor.gearTier = authored.armorGearTier;
    candidate.item.instance.armor.masterworkLevel = authored.armorMasterworkLevel;
    // Any authored armor meta is reported at the source, so a value that never reaches the stats
    // phase can be placed either side of this copy rather than guessed at.
    if (authored.armorArchetype != 0 || authored.armorGearTier != 0
        || authored.armorMasterworkLevel != 0
        || authored.armorSetHash != authored_inventory::kNoDefinitionHash) {
        std::array<char, 192> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=armor_meta stage=authored hash=0x%08X a=%u t=%u "
                                          "m=%u s=%x",
                                          authored.definitionHash,
                                          static_cast<unsigned>(authored.armorArchetype),
                                          static_cast<unsigned>(authored.armorGearTier),
                                          static_cast<unsigned>(authored.armorMasterworkLevel),
                                          authored.armorSetHash);
        if (written > 0) {
            core::log::write(core::log::Channel::middleware,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    // State spells "no set" as the FNV basis its authored rows default to, while ArmorMeta spells
    // it as zero like its other three counters. Translating here keeps one meaning of "none" for
    // everything downstream, so a later consumer can test the whole struct against zero.
    candidate.item.instance.armor.setHash =
        authored.armorSetHash == authored_inventory::kNoDefinitionHash ? 0U
                                                                      : authored.armorSetHash;
    candidate.item.instance.curveSelector = instance::layout::kInitialLevelCurveX;
    candidate.item.instance.capSelector = instance::layout::kInitialLevelCapRow;
    candidate.item.instance.socketEntryListIndex = socketList.definitionIndex;
    candidate.item.instance.socketEntryCount = socketList.entryCount;
    candidate.item.instance.socketEntryContentsResolved = true;
    resolve_socket_states(socketList,
                          authored,
                          character.characterClass,
                          character.acquiredSubclassAbilityMask,
                          candidate.item.instance.socketEntryStates,
                          candidate.item.instance.socketSelectors);
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::datagen::family4::loadout
