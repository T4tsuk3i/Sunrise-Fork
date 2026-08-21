/** Character creation and deletion logic. */

#include "state_account_roster_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace {

void report_roster(std::string_view event) noexcept {
    core::log::write(core::log::Channel::state, core::log::Level::warn, event);
}

/**
 * Copies the settings-authored default loadout for this character's class onto it, so a newly
 * created character is not stuck at power 0 with no gear or emblem.
 *
 * Every copied item is given a freshly allocated instance SOID instead of the authored
 * template's fixed one: the template reuses the same SOIDs for every character of a given
 * class, which collides once more than one character of that class has ever existed on the
 * account (exactly the create-after-delete flow this is meant to support).
 * @param account In-out account; scanned for collision-free SOIDs as each item is assigned one.
 * @param character In-out character, already appended to account.characters and counted.
 */
void seed_default_loadout(AccountState& account, CharacterState& character) noexcept {
    const CharacterState* authored = nullptr;
    for (const CharacterState& candidate : core::settings::get().initialAccount.characters) {
        if (candidate.characterClass == character.characterClass) {
            authored = &candidate;
            break;
        }
    }
    if (authored == nullptr) {
        char buf[96]{};
        const int n = std::snprintf(buf,
                                    sizeof buf,
                                    "ev=create_character stage=loadout result=fail "
                                    "reason=no_template class=%u",
                                    static_cast<unsigned>(character.characterClass));
        if (n > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             {buf, static_cast<std::size_t>(n)});
        }
        return;
    }
    character.level = authored->level;
    character.accepted = authored->accepted;
    character.previewAvailable = authored->previewAvailable;
    character.appearanceValue = authored->appearanceValue;
    character.lastOrbitedDestination = authored->lastOrbitedDestination;
    character.contentBypass = authored->contentBypass;
    character.equipment = authored->equipment;
    character.inventory = authored->inventory;
    // The ability entries ride on the subclass item inside the equipment copied above, so they
    // arrive already matched to the subclass they were authored against. Copying them per
    // character, as this did before they moved onto the item, is what used to let one class's
    // selection reach another class's socket layout.
    for (auto& slot : character.equipment.slots) {
        if (!slot.has_value()) {
            continue;
        }
        std::uint64_t freshSoid = 0;
        if (!runtime::detail::next_item_instance_soid(account, freshSoid)) {
            character.equipment = {};
            character.inventory = {};
            character.nextInventorySerial = 0;
            report_roster("ev=create_character stage=loadout result=fail reason=soid_exhausted");
            return;
        }
        slot->instanceSoid = freshSoid;
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        std::uint64_t freshSoid = 0;
        if (!runtime::detail::next_item_instance_soid(account, freshSoid)) {
            character.equipment = {};
            character.inventory = {};
            character.nextInventorySerial = 0;
            report_roster("ev=create_character stage=loadout result=fail reason=soid_exhausted");
            return;
        }
        character.inventory.values[index].instanceSoid = freshSoid;
    }
    // Every resolved item must satisfy mutationSerial < nextInventorySerial (the family-4
    // character-object encoder enforces this). The authored template's items all carry the
    // JSON-default mutationSerial of 0, and its own nextInventorySerial is unset (also 0) --
    // copying that pair verbatim fails the strict "<" the moment the character is actually
    // selected to play, so it is computed fresh from the copied items instead.
    std::int32_t maxMutationSerial = -1;
    for (const auto& slot : character.equipment.slots) {
        if (slot.has_value()) {
            maxMutationSerial = (std::max)(maxMutationSerial, slot->mutationSerial);
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        maxMutationSerial =
            (std::max)(maxMutationSerial, character.inventory.values[index].mutationSerial);
    }
    const auto itemCount = [&] {
        std::size_t count = 0;
        for (const auto& slot : character.equipment.slots) {
            if (slot.has_value()) {
                ++count;
            }
        }
        return count + character.inventory.count;
    }();
    character.nextInventorySerial = static_cast<std::uint32_t>(
        (std::max)(maxMutationSerial + 1, static_cast<std::int32_t>(itemCount)));
}

} // namespace

bool create_character(AccountState& account,
                      std::uint8_t characterClass,
                      std::uint8_t gender,
                      std::uint8_t race,
                      std::uint64_t& characterSoid) noexcept {
    characterSoid = 0;
    {
        char buf[160]{};
        const int n = std::snprintf(buf,
                                    sizeof buf,
                                    "ev=create_character stage=request class=%u gender=%u race=%u "
                                    "chars=%zu cap=%zu primary=0x%016llX",
                                    static_cast<unsigned>(characterClass),
                                    static_cast<unsigned>(gender),
                                    static_cast<unsigned>(race),
                                    account.characterCount,
                                    account.characters.size(),
                                    static_cast<unsigned long long>(account.primarySoid));
        if (n > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             {buf, static_cast<std::size_t>(n)});
        }
    }
    if (account.characterCount >= account.characters.size()) {
        report_roster("ev=create_character stage=capacity result=fail");
        return false;
    }
    if (characterClass > static_cast<std::uint8_t>(CharacterClass::warlock)
        || gender > static_cast<std::uint8_t>(CharacterGender::female)
        || race > static_cast<std::uint8_t>(CharacterRace::exo)) {
        char buf[160]{};
        const int n =
            std::snprintf(buf,
                          sizeof buf,
                          "ev=create_character stage=range result=fail class=%u gender=%u race=%u",
                          static_cast<unsigned>(characterClass),
                          static_cast<unsigned>(gender),
                          static_cast<unsigned>(race));
        if (n > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             {buf, static_cast<std::size_t>(n)});
        }
        return false;
    }
    if (account.primarySoid == 0) {
        report_roster("ev=create_character stage=primary result=fail");
        return false;
    }

    const std::size_t index = account.characterCount;
    // The slot index cannot name the SOID: deletion keeps every survivor's SOID, so after
    // deleting a middle character the next free index already belongs to a living one. The lowest
    // unused offset is taken instead, which reuses a freed SOID without ever colliding.
    for (std::uint64_t offset = 1U; offset <= account.characters.size(); ++offset) {
        const std::uint64_t candidate = account.primarySoid + offset;
        bool taken = false;
        for (std::size_t existing = 0; existing < account.characterCount; ++existing) {
            if (account.characters[existing].soid == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            characterSoid = candidate;
            break;
        }
    }
    if (characterSoid == 0) {
        report_roster("ev=create_character stage=soid result=fail");
        return false;
    }

    CharacterState& character = account.characters[index];
    character = {};
    character.soid = characterSoid;
    character.race = static_cast<CharacterRace>(race);
    character.gender = static_cast<CharacterGender>(gender);
    character.characterClass = static_cast<CharacterClass>(characterClass);
    character.level = 0;
    character.accepted = false;
    character.previewAvailable = false;
    character.appearanceValue = 0.0f;
    character.lastOrbitedDestination = 0;
    character.contentBypass = false;
    character.equipment = {};
    character.inventory = {};
    character.nextInventorySerial = 0;

    // Select the new character and deselect all others.
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        account.characters[i].selected = false;
    }
    character.selected = true;
    ++account.characterCount;

    // Counted above so the fresh-SOID scan inside sees this character's own items as they are
    // assigned, not just the characters that existed before it.
    seed_default_loadout(account, character);
    {
        std::size_t equippedCount = 0;
        for (const auto& slot : character.equipment.slots) {
            if (slot.has_value()) {
                ++equippedCount;
            }
        }
        char buf[224]{};
        const int n = std::snprintf(buf,
                                    sizeof buf,
                                    "ev=create_character stage=seeded soid=0x%016llX level=%u "
                                    "accepted=%d preview=%d appearance=%.2f equipped=%zu "
                                    "inventory=%zu next_serial=%u",
                                    static_cast<unsigned long long>(character.soid),
                                    static_cast<unsigned>(character.level),
                                    static_cast<int>(character.accepted),
                                    static_cast<int>(character.previewAvailable),
                                    static_cast<double>(character.appearanceValue),
                                    equippedCount,
                                    character.inventory.count,
                                    character.nextInventorySerial);
        if (n > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             {buf, static_cast<std::size_t>(n)});
        }
    }

    if (!account::valid(account)) {
        char buf[256]{};
        const int n =
            std::snprintf(buf,
                          sizeof buf,
                          "ev=create_character stage=validate result=fail primary=0x%016llX "
                          "chars=%zu profileItems=%zu dismantleRewards=%zu settings.configured=%d",
                          static_cast<unsigned long long>(account.primarySoid),
                          account.characterCount,
                          account.profileItemCount,
                          account.dismantleRewardCount,
                          static_cast<int>(account.settings.configured));
        if (n > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             {buf, static_cast<std::size_t>(n)});
        }
        --account.characterCount;
        character = {};
        characterSoid = 0;
        return false;
    }
    {
        char buf[128]{};
        const int n =
            std::snprintf(buf,
                          sizeof buf,
                          "ev=create_character stage=commit result=ok soid=0x%016llX chars=%zu",
                          static_cast<unsigned long long>(characterSoid),
                          account.characterCount);
        if (n > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             {buf, static_cast<std::size_t>(n)});
        }
    }
    return true;
}

bool delete_character(AccountState& account, std::uint64_t characterSoid) noexcept {
    if (characterSoid == 0 || account.characterCount == 0) {
        return false;
    }
    if (account.primarySoid == 0) {
        return false;
    }

    std::size_t found = account.characterCount;
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        if (account.characters[i].soid == characterSoid) {
            found = i;
            break;
        }
    }
    if (found == account.characterCount) {
        return false;
    }

    // Survivors keep the SOID they were created with. Rebasing them onto their new slot index
    // silently renames living characters, which strands every object the client already holds
    // under the old SOID -- its roster entry, its Family-4 body and its banner all keyed by it.
    for (std::size_t i = found; i + 1 < account.characterCount; ++i) {
        account.characters[i] = account.characters[i + 1U];
    }
    --account.characterCount;
    account.characters[account.characterCount] = {};

    // Select the first character if the deleted one was selected.
    bool hasSelection = false;
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        if (account.characters[i].selected) {
            hasSelection = true;
            break;
        }
    }
    if (!hasSelection && account.characterCount > 0) {
        account.characters[0].selected = true;
    }

    if (!account::valid(account)) {
        return false;
    }
    return true;
}

} // namespace sunrise::state
