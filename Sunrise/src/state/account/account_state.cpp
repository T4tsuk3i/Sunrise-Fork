#include "account_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "../../core/logging/log.h"

namespace sunrise::state::account {
namespace {

/**
 * Logs which specific check inside valid_impl rejected the account. Every caller upstream (state
 * boot, persistence load) otherwise only learns "invalid" with no reason, which turns any real
 * rejection into a silent, unexplained boot failure.
 * @param reason Short key naming the failing check.
 * @param detail Optional formatted detail (e.g. an index or hash) appended after reason.
 * @return False, for a direct return.
 */
[[nodiscard]] bool report_invalid(const char* reason, const char* detail = "") noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=account_valid stage=%s result=fail%s%s",
                                      reason,
                                      detail[0] != '\0' ? " " : "",
                                      detail);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/** Enough fixed storage for every account, character, profile-stack, and character-item key. */
inline constexpr std::size_t kIdentityCapacity =
    1 + inventory::kProfileItemCapacity
    + kCharacterCapacity * (1 + inventory::kEquipmentSlotCount + inventory::kCharacterItemCapacity);

/** @return True when a profile row carries no authored or runtime-owned value. */
[[nodiscard]] bool empty_profile_item(const inventory::ProfileItem& item) noexcept {
    return item.instanceSoid == 0 && item.definitionHash == 0 && item.quantity == 0
           && item.mutationSerial == 0;
}

/** Tier bits 1-5 are the native rarity ladder; bit 0 (no tier) is never a payout target. */
constexpr std::uint8_t kDismantleTierMaskBits = 0b0011'1110U;
constexpr std::uint8_t kDismantleClassMaskBits =
    static_cast<std::uint8_t>(DismantleGearClass::weapon)
    | static_cast<std::uint8_t>(DismantleGearClass::armor);

/** @return True when one unused dismantle policy row is canonical zero. */
[[nodiscard]] bool empty_dismantle_reward(const DismantleRewardPolicy& reward) noexcept {
    return reward.definitionHash == 0 && reward.quantity == 0 && reward.tierMask == 0
           && reward.classMask == 0 && reward.masterwork == DismantleMasterworkFilter::any;
}

/** Checks filled policy rows, uniqueness, and the zero tail. */
[[nodiscard]] bool valid_dismantle_rewards(const AccountState& state) noexcept {
    if (state.dismantleRewardCount > state.dismantleRewards.size()) {
        return false;
    }
    for (std::size_t index = 0; index < state.dismantleRewards.size(); ++index) {
        const DismantleRewardPolicy& reward = state.dismantleRewards[index];
        if (index >= state.dismantleRewardCount) {
            if (!empty_dismantle_reward(reward)) {
                return false;
            }
            continue;
        }
        if (reward.definitionHash == inventory::kNoDefinitionHash || reward.quantity <= 0
            || (reward.tierMask & ~kDismantleTierMaskBits) != 0
            || (reward.classMask & ~kDismantleClassMaskBits) != 0
            || reward.masterwork > DismantleMasterworkFilter::notMasterworked) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (same_dismantle_policy_key(state.dismantleRewards[prior], reward)) {
                return false;
            }
        }
    }
    return true;
}

/** Adds one nonzero globally unique key to a bounded identity set. */
[[nodiscard]] bool append_identity(std::array<std::uint64_t, kIdentityCapacity>& identities,
                                   std::size_t& count,
                                   std::uint64_t soid) noexcept {
    if (soid == 0 || count >= identities.size()) {
        return false;
    }
    const auto end = identities.cbegin() + static_cast<std::ptrdiff_t>(count);
    if (std::find(identities.cbegin(), end, soid) != end) {
        return false;
    }
    identities[count++] = soid;
    return true;
}

/** Checks the complete authored/runtime structure without consulting installed build data. */
[[nodiscard]] bool valid_impl(const AccountState& state) noexcept {
    if (state.profileItemCount > state.profileItems.size()
        || state.characterCount > state.characters.size()) {
        return report_invalid("count_capacity");
    }
    if (state.primarySoid == 0) {
        if (state.profileItemCount != 0 || state.characterCount != 0
            || state.dismantleRewardCount != 0 || state.settings.configured
            || state.settings.keyBindings.configured) {
            return report_invalid("empty_account_nonzero_fields");
        }
        if (!std::all_of(
                state.profileItems.cbegin(), state.profileItems.cend(), empty_profile_item)) {
            return report_invalid("empty_account_stale_profile_item");
        }
        if (!std::all_of(state.dismantleRewards.cbegin(),
                         state.dismantleRewards.cend(),
                         empty_dismantle_reward)) {
            return report_invalid("empty_account_stale_dismantle_reward");
        }
        return true;
    }
    if (!settings::valid(state.settings)) {
        return report_invalid("settings");
    }
    if (!valid_dismantle_rewards(state)) {
        return report_invalid("dismantle_rewards");
    }

    std::array<std::uint64_t, kIdentityCapacity> identities{};
    std::size_t identityCount = 0;
    if (!append_identity(identities, identityCount, state.primarySoid)) {
        return report_invalid("primary_soid");
    }
    for (std::size_t index = 0; index < state.profileItems.size(); ++index) {
        const inventory::ProfileItem& item = state.profileItems[index];
        char detail[32]{};
        std::snprintf(detail, sizeof detail, "index=%zu", index);
        if (index >= state.profileItemCount) {
            if (!empty_profile_item(item)) {
                return report_invalid("profile_item_tail_not_empty", detail);
            }
            continue;
        }
        if (item.definitionHash == inventory::kNoDefinitionHash) {
            return report_invalid("profile_item_definition_hash", detail);
        }
        if (item.quantity <= 0) {
            return report_invalid("profile_item_quantity", detail);
        }
        if (item.mutationSerial < 0) {
            return report_invalid("profile_item_mutation_serial", detail);
        }
        if (item.instanceSoid != 0
            && !append_identity(identities, identityCount, item.instanceSoid)) {
            return report_invalid("profile_item_instance_soid_duplicate", detail);
        }
    }

    bool selected = false;
    for (std::size_t index = 0; index < state.characterCount; ++index) {
        const CharacterState& character = state.characters[index];
        char detail[32]{};
        std::snprintf(detail, sizeof detail, "char=%zu", index);
        if (!append_identity(identities, identityCount, character.soid)) {
            return report_invalid("character_soid_duplicate", detail);
        }
        if (character.selected && selected) {
            return report_invalid("character_multiple_selected", detail);
        }
        if (character.race > CharacterRace::exo) {
            return report_invalid("character_race_range", detail);
        }
        if (character.gender > CharacterGender::female) {
            return report_invalid("character_gender_range", detail);
        }
        if (character.characterClass > CharacterClass::warlock) {
            return report_invalid("character_class_range", detail);
        }
        if (!std::isfinite(character.appearanceValue)) {
            return report_invalid("character_appearance_value", detail);
        }
        if (!inventory::valid(character.equipment)) {
            return report_invalid("character_equipment", detail);
        }
        if (!inventory::valid(character.inventory)) {
            return report_invalid("character_inventory", detail);
        }
        selected = selected || character.selected;
        for (const std::optional<inventory::Item>& item : character.equipment.slots) {
            if (item.has_value()
                && !append_identity(identities, identityCount, item->instanceSoid)) {
                return report_invalid("character_equipment_instance_soid_duplicate", detail);
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
            if (!append_identity(identities,
                                 identityCount,
                                 character.inventory.values[itemIndex].instanceSoid)) {
                return report_invalid("character_inventory_instance_soid_duplicate", detail);
            }
        }
    }
    return true;
}

} // namespace

/**
 * Checks bounded ids and whole settings for every nonzero account.
 * @param state Account State to check.
 * @return True for empty State, or a whole account with unique ids.
 */
bool valid(const AccountState& state) noexcept {
    return valid_impl(state);
}

/** Checks settings-authored State before runtime-only profile stack identities are seeded. */
bool valid_authored(const AccountState& state) noexcept {
    return valid_impl(state);
}

/**
 * Finds the selected character without storing a second account-level key.
 * @param state Account snapshot read under the lock.
 * @return The selected character's nonzero SOID, or zero when none is selected.
 */
std::uint64_t selected_character_soid(const AccountState& state) noexcept {
    const std::size_t count = (std::min)(state.characterCount, state.characters.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (state.characters[index].selected) {
            return state.characters[index].soid;
        }
    }
    return 0;
}

/**
 * Finds the character the family-zero banner pair names.
 * The pair goes out before any pick, so it falls back to the first character.
 * @param state Account snapshot read under the lock.
 * @return The character's nonzero SOID, or zero when the account owns none.
 */
std::uint64_t banner_character_soid(const AccountState& state) noexcept {
    const std::uint64_t selected = selected_character_soid(state);
    if (selected != 0) {
        return selected;
    }
    return state.characterCount == 0 ? 0 : state.characters[0].soid;
}

} // namespace sunrise::state::account
