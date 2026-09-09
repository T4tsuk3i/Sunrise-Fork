/**
 * Dumps the published game-data tables that name the character's abilities.
 *
 * A socket entry list is the subclass's ability layout: every entry names one selectable node,
 * its group holds the alternatives (so the class-ability group carries both the Gambler's and the
 * Marksman's Dodge), and each entry's pool routes definition hashes into the twelve semantic
 * buckets the record publishes. Those hashes, their item definitions, and the equipped exotic
 * helmet's detail are all content Sunrise has already extracted, so this audits the data instead
 * of probing the sandbox. One enabled run prints the whole layout and stops.
 */

#include "ability_audit.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::client::diagnostics {
namespace {

/** Retry gap while the content domains are still being extracted at boot. */
constexpr DWORD kRetryIntervalMilliseconds = 500;
/** Retries before the audit gives up on the content domains, roughly a minute of boot time. */
constexpr std::size_t kRetryLimit = 120;
/** Retries spent waiting for a character to equip an exotic helmet, about two minutes. */
constexpr std::size_t kHelmetWaitLimit = 240;
/** Semantic buckets (grenade, super, melee, movement, sprint, class) whose hashes resolve. */
constexpr std::size_t kSemanticBucketCount = 6;
/** Native inventory bucket id of the helmet, matching the equipment-slot enum index. */
constexpr std::uint8_t kHelmetBucket = 3;
/** Buffer backing one emitted line. */
constexpr std::size_t kLineCapacity = 512;

/** Writes one prepared line. */
void emit(std::string_view line) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::info, line);
}

/** Appends one `,value` to a line buffer, tracking the used length. */
void append_value(std::array<char, kLineCapacity>& line,
                  std::size_t& used,
                  const char* prefix,
                  std::uint32_t value) noexcept {
    if (used >= line.size()) {
        return;
    }
    const int written = std::snprintf(line.data() + used,
                                      line.size() - used,
                                      "%s%u",
                                      prefix,
                                      static_cast<unsigned>(value));
    if (written > 0) {
        used += static_cast<std::size_t>(written);
    }
}

/** Emits every hash of one published ability bucket, one comma-separated line per bucket. */
void report_bucket(const state::build_data::abilities::Definition& row) noexcept {
    for (std::size_t index = 0; index < state::build_data::abilities::kBucketCapacity; ++index) {
        const state::build_data::abilities::Bucket& bucket = row.buckets[index];
        if (bucket.kind == state::build_data::abilities::kEmptyBucketKind) {
            continue;
        }
        std::array<char, kLineCapacity> line{};
        std::size_t used = 0;
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=ability_audit stage=bucket b=%zu kind=%u "
                                          "n=%u hashes=",
                                          index,
                                          static_cast<unsigned>(bucket.kind),
                                          static_cast<unsigned>(bucket.hashCount));
        if (written > 0) {
            used = static_cast<std::size_t>(written);
        }
        for (std::size_t hash = 0; hash < bucket.hashCount; ++hash) {
            append_value(line, used, hash == 0 ? "" : ",", bucket.hashes[hash]);
        }
        emit({line.data(), used});
    }
}

/** Appends one detail's stat rows to a `rows=` line. */
void append_stat_rows(std::array<char, kLineCapacity>& line,
                      std::size_t& used,
                      const state::build_data::items::details::Definition& detail) noexcept {
    std::array<char, 16> row{};
    for (std::size_t index = 0; index < detail.statCount && index < detail.stats.size(); ++index) {
        const int written = std::snprintf(row.data(),
                                          row.size(),
                                          index == 0 ? "%u:%d" : ",%u:%d",
                                          static_cast<unsigned>(detail.stats[index].row),
                                          static_cast<int>(detail.stats[index].value));
        if (written > 0 && used + static_cast<std::size_t>(written) < line.size()) {
            std::memcpy(line.data() + used, row.data(), static_cast<std::size_t>(written));
            used += static_cast<std::size_t>(written);
        }
    }
}

/** Logs one definition hash's identity and, when present, its configured item detail. */
void report_ability(std::uint32_t definitionHash) noexcept {
    state::build_data::items::Definition item{};
    std::array<char, kLineCapacity> line{};
    if (!state::build_data::find_item_definition_hash(definitionHash, item)) {
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=ability_audit stage=ability hash=0x%08x result=missing",
                                          static_cast<unsigned>(definitionHash));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }
    state::build_data::items::details::Definition detail{};
    std::uint16_t entryList = 0;
    std::uint8_t statCount = 0;
    if (state::build_data::find_configured_item_detail(item.definitionIndex, detail)) {
        entryList = detail.socketEntryListIndex;
        statCount = detail.statCount;
    }
    const int identity = std::snprintf(line.data(),
                                       line.size(),
                                       "ev=ability_audit stage=ability hash=0x%08x -> idx=%u "
                                       "bucket=%u tier=%u plugcat=0x%08x list=%u stats=%u",
                                       static_cast<unsigned>(definitionHash),
                                       static_cast<unsigned>(item.definitionIndex),
                                       static_cast<unsigned>(item.bucketId),
                                       static_cast<unsigned>(item.tier),
                                       static_cast<unsigned>(item.plugCategoryHash),
                                       static_cast<unsigned>(entryList),
                                       static_cast<unsigned>(statCount));
    if (identity > 0) {
        emit({line.data(), static_cast<std::size_t>(identity)});
    }
    if (!state::build_data::find_configured_item_detail(item.definitionIndex, detail)) {
        return;
    }
    if (detail.statCount != 0) {
        std::array<char, kLineCapacity> statLine{};
        std::size_t used = 0;
        const int header = std::snprintf(statLine.data(),
                                         statLine.size(),
                                         "ev=ability_audit stage=ability_stats hash=0x%08x rows=",
                                         static_cast<unsigned>(definitionHash));
        if (header > 0) {
            used = static_cast<std::size_t>(header);
        }
        append_stat_rows(statLine, used, detail);
        emit({statLine.data(), used});
    }
    if (detail.sandboxPerkCount != 0) {
        std::array<char, kLineCapacity> perkLine{};
        std::size_t used = 0;
        const int header = std::snprintf(perkLine.data(),
                                         perkLine.size(),
                                         "ev=ability_audit stage=ability_perks hash=0x%08x "
                                         "perks=",
                                         static_cast<unsigned>(definitionHash));
        if (header > 0) {
            used = static_cast<std::size_t>(header);
        }
        for (std::size_t index = 0;
             index < detail.sandboxPerkCount && index < detail.sandboxPerks.size();
             ++index) {
            append_value(perkLine, used, index == 0 ? "" : ",", detail.sandboxPerks[index]);
        }
        emit({perkLine.data(), used});
    }
}

/** Logs the latent socket-entry census one subclass list publishes, entry by entry. */
void report_entry_census(std::size_t character,
                         std::uint16_t list,
                         std::size_t entryCount) noexcept {
    std::array<char, kLineCapacity> censusLine{};
    const int census = std::snprintf(censusLine.data(),
                                     censusLine.size(),
                                     "ev=ability_audit stage=entries char=%zu list=%u n=%zu",
                                     character,
                                     static_cast<unsigned>(list),
                                     entryCount);
    if (census > 0) {
        emit({censusLine.data(), static_cast<std::size_t>(census)});
    }
    state::build_data::socket_entry_lists::EntryTable entryTable{};
    if (!state::build_data::find_socket_entry_table(list, entryTable)) {
        emit("ev=ability_audit stage=entries result=no_table");
        return;
    }
    std::array<char, kLineCapacity> entryLine{};
    for (std::size_t index = 0; index < entryCount; ++index) {
        const state::build_data::socket_entry_lists::Entry& entry = entryTable.entries[index];
        const int written = std::snprintf(entryLine.data(),
                                          entryLine.size(),
                                          "ev=ability_audit stage=entry i=%zu group=%u kind=%u "
                                          "plug=0x%08x",
                                          index,
                                          static_cast<unsigned>(entry.group),
                                          static_cast<unsigned>(entry.kind),
                                          static_cast<unsigned>(entry.plugSource));
        if (written > 0) {
            emit({entryLine.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Resolves every hash the character's own semantic ability buckets carry. */
void report_semantic_abilities(const state::build_data::abilities::Definition& row,
                               std::uint8_t classBucket) noexcept {
    const std::array<int, kSemanticBucketCount> buckets{
        0, 1, 2, 3, 4, static_cast<int>(classBucket),
    };
    for (std::size_t which = 0; which < kSemanticBucketCount; ++which) {
        const state::build_data::abilities::Bucket& bucket =
            row.buckets[static_cast<std::size_t>(buckets[which])];
        for (std::size_t hash = 0; hash < bucket.hashCount; ++hash) {
            report_ability(bucket.hashes[hash]);
        }
    }
}

/** Logs one character's subclass, selection, published buckets and socket-entry census. */
void report_character(std::size_t character, const state::CharacterState& characterState) noexcept {
    std::array<char, kLineCapacity> line{};
    const int characterHeader = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=ability_audit stage=character char=%zu class=%u",
                                              character,
                                              static_cast<unsigned>(characterState.characterClass));
    if (characterHeader > 0) {
        emit({line.data(), static_cast<std::size_t>(characterHeader)});
    }

    const auto& slots = characterState.equipment.slots;
    const auto subclass =
        slots[static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass)];
    if (!subclass.has_value()) {
        emit("ev=ability_audit stage=subclass result=no_subclass");
        return;
    }
    state::build_data::items::Definition subclassItem{};
    state::build_data::items::details::Definition subclassDetail{};
    std::uint16_t entryList = 0;
    if (state::build_data::find_item_definition_hash(subclass->definitionHash, subclassItem)
        && state::build_data::find_configured_item_detail(subclassItem.definitionIndex,
                                                          subclassDetail)) {
        entryList = subclassDetail.socketEntryListIndex;
    }
    const int subclassHeader = std::snprintf(
        line.data(),
        line.size(),
        "ev=ability_audit stage=subclass char=%zu hash=0x%08x idx=%u bucket=%u tier=%u list=%u",
        character,
        static_cast<unsigned>(subclass->definitionHash),
        static_cast<unsigned>(subclassItem.definitionIndex),
        static_cast<unsigned>(subclassItem.bucketId),
        static_cast<unsigned>(subclassItem.tier),
        static_cast<unsigned>(entryList));
    if (subclassHeader > 0) {
        emit({line.data(), static_cast<std::size_t>(subclassHeader)});
    }

    state::build_data::abilities::Selection selection{subclass->movementAbilityEntry,
                                                       subclass->grenadeAbilityEntry,
                                                       subclass->superAbilityEntry,
                                                       subclass->meleeAbilityEntry,
                                                       subclass->classAbilityEntry};
    std::array<char, kLineCapacity> selectionLine{};
    const int selectionHeader = std::snprintf(
        selectionLine.data(),
        selectionLine.size(),
        "ev=ability_audit stage=selection char=%zu movement=%u grenade=%u super=%u melee=%u "
        "class=%u",
        character,
        static_cast<unsigned>(selection.movementEntry),
        static_cast<unsigned>(selection.grenadeEntry),
        static_cast<unsigned>(selection.superEntry),
        static_cast<unsigned>(selection.meleeEntry),
        static_cast<unsigned>(selection.classEntry));
    if (selectionHeader > 0) {
        emit({selectionLine.data(), static_cast<std::size_t>(selectionHeader)});
    }

    if (entryList == 0) {
        emit("ev=ability_audit stage=subclass result=no_list");
    } else {
        state::build_data::abilities::Definition row{};
        if (state::build_data::find_ability_buckets(entryList, selection, row)) {
            report_bucket(row);
            report_semantic_abilities(
                row, state::class_ability_bucket(characterState.characterClass));
        }
        state::build_data::socket_entry_lists::Definition listDefinition{};
        const std::size_t entryCount =
            state::build_data::find_socket_entry_list(entryList, listDefinition)
                ? listDefinition.entryCount
                : 0;
        if (entryCount != 0) {
            report_entry_census(character, entryList, entryCount);
        }
    }

    const auto helmet =
        slots[static_cast<std::size_t>(state::account::inventory::EquipmentSlot::helmet)];
    if (!helmet.has_value()) {
        emit("ev=ability_audit stage=helmet result=absent");
        return;
    }
    state::build_data::items::Definition helmetItem{};
    if (!state::build_data::find_item_definition_hash(helmet->definitionHash, helmetItem)) {
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=ability_audit stage=helmet char=%zu hash=0x%08x "
                                          "result=missing",
                                          character,
                                          static_cast<unsigned>(helmet->definitionHash));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }
    state::build_data::items::details::Definition helmetDetail{};
    if (!state::build_data::find_configured_item_detail(helmetItem.definitionIndex, helmetDetail)) {
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=ability_audit stage=helmet char=%zu hash=0x%08x "
                                          "result=no_detail",
                                          character,
                                          static_cast<unsigned>(helmet->definitionHash));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }
    const int helmetHeader = std::snprintf(
        line.data(),
        line.size(),
        "ev=ability_audit stage=helmet char=%zu hash=0x%08x -> idx=%u bucket=%u tier=%u "
        "plugcat=0x%08x list=%u stats=%u perks=%u sockets=%u",
        character,
        static_cast<unsigned>(helmet->definitionHash),
        static_cast<unsigned>(helmetItem.definitionIndex),
        static_cast<unsigned>(helmetItem.bucketId),
        static_cast<unsigned>(helmetItem.tier),
        static_cast<unsigned>(helmetItem.plugCategoryHash),
        static_cast<unsigned>(helmetDetail.socketEntryListIndex),
        static_cast<unsigned>(helmetDetail.statCount),
        static_cast<unsigned>(helmetDetail.sandboxPerkCount),
        static_cast<unsigned>(helmetDetail.ordinarySocketCount));
    if (helmetHeader > 0) {
        emit({line.data(), static_cast<std::size_t>(helmetHeader)});
    }
    if (helmetDetail.statCount != 0) {
        std::array<char, kLineCapacity> statLine{};
        std::size_t used = 0;
        const int header = std::snprintf(statLine.data(),
                                         statLine.size(),
                                         "ev=ability_audit stage=helmet_stats hash=0x%08x rows=",
                                         static_cast<unsigned>(helmet->definitionHash));
        if (header > 0) {
            used = static_cast<std::size_t>(header);
        }
        append_stat_rows(statLine, used, helmetDetail);
        emit({statLine.data(), used});
    }
    if (helmetDetail.sandboxPerkCount != 0) {
        std::array<char, kLineCapacity> perkLine{};
        std::size_t used = 0;
        const int header = std::snprintf(perkLine.data(),
                                         perkLine.size(),
                                         "ev=ability_audit stage=helmet_perks hash=0x%08x "
                                         "perks=",
                                         static_cast<unsigned>(helmet->definitionHash));
        if (header > 0) {
            used = static_cast<std::size_t>(header);
        }
        for (std::size_t index = 0;
             index < helmetDetail.sandboxPerkCount && index < helmetDetail.sandboxPerks.size();
             ++index) {
            append_value(perkLine, used, index == 0 ? "" : ",", helmetDetail.sandboxPerks[index]);
        }
        emit({perkLine.data(), used});
    }
}

/** @return True when any character's helmet slot holds a resolved exotic (tier 5) definition. */
bool exotic_helmet_equipped(const state::AccountState& account) noexcept {
    for (std::size_t character = 0; character < account.characterCount; ++character) {
        const auto& helmet =
            account.characters[character]
                .equipment
                .slots[static_cast<std::size_t>(
                    state::account::inventory::EquipmentSlot::helmet)];
        if (!helmet.has_value()) {
            continue;
        }
        state::build_data::items::Definition item{};
        if (state::build_data::find_item_definition_hash(helmet->definitionHash, item)
            && item.tier == static_cast<std::uint8_t>(state::build_data::items::Tier::exotic)
            && item.bucketId == kHelmetBucket) {
            return true;
        }
    }
    return false;
}

/** Emits every exotic helmet definition the installed build carries, in native-index order. */
void report_exotic_helmets() noexcept {
    const std::size_t count = state::build_data::item_definition_count();
    std::array<char, kLineCapacity> header{};
    const int written = std::snprintf(
        header.data(), header.size(), "ev=ability_audit stage=exotic_helmets n=%zu", count);
    if (written > 0) {
        emit({header.data(), static_cast<std::size_t>(written)});
    }
    for (std::size_t index = 0; index < count; ++index) {
        state::build_data::items::Definition item{};
        if (!state::build_data::find_item_definition_index(
                static_cast<std::uint16_t>(index), item)) {
            continue;
        }
        if (item.bucketId == kHelmetBucket
            && item.tier == static_cast<std::uint8_t>(state::build_data::items::Tier::exotic)) {
            report_ability(item.definitionHash);
        }
    }
}

/** Emits each character's current helmet slot hash and resolved tier so equip timing is visible. */
void report_helmet_poll() noexcept {
    const state::AccountState account = state::account_snapshot();
    for (std::size_t character = 0; character < account.characterCount; ++character) {
        const auto& helmet =
            account.characters[character]
                .equipment
                .slots[static_cast<std::size_t>(
                    state::account::inventory::EquipmentSlot::helmet)];
        std::array<char, kLineCapacity> line{};
        int written = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=ability_audit stage=helmet_poll char=%zu result=absent",
                                    character);
        if (helmet.has_value()) {
            state::build_data::items::Definition item{};
            if (!state::build_data::find_item_definition_hash(helmet->definitionHash, item)) {
                written = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=ability_audit stage=helmet_poll char=%zu hash=0x%08x result=missing",
                                        character,
                                        helmet->definitionHash);
            } else {
                written = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=ability_audit stage=helmet_poll char=%zu hash=0x%08x idx=%u bucket=%u tier=%u",
                    character,
                    helmet->definitionHash,
                    item.definitionIndex,
                    item.bucketId,
                    item.tier);
            }
        }
        if (written > 0 && written < static_cast<int>(line.size())) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Waits for the content domains, then dumps every configured character's ability data. */
DWORD WINAPI audit_thread(LPVOID) noexcept {
    for (std::size_t attempt = 0; attempt < kRetryLimit; ++attempt) {
        if (state::build_data::item_definitions_ready()
            && state::build_data::socket_entry_lists_ready()
            && state::build_data::ability_buckets_ready()
            && state::build_data::socket_entry_buckets_ready()) {
            break;
        }
        Sleep(kRetryIntervalMilliseconds);
    }
    if (!state::build_data::item_definitions_ready()
        || !state::build_data::socket_entry_lists_ready()
        || !state::build_data::ability_buckets_ready()
        || !state::build_data::socket_entry_buckets_ready()) {
        emit("ev=ability_audit stage=begin result=domains_not_ready");
        return 0;
    }
    // The dump is one shot, and its helmet stage only shows what is equipped when it fires. A boot
    // that starts in orbit would dump before the player swaps Nighthawk on, so wait until an exotic
    // helmet is actually equipped rather than firing the moment the content domains are ready.
    std::size_t attempts = 0;
    while (attempts++ < kHelmetWaitLimit && !exotic_helmet_equipped(state::account_snapshot())) {
        if (attempts % 10 == 1) {
            report_helmet_poll();
        }
        Sleep(kRetryIntervalMilliseconds);
    }
    if (!exotic_helmet_equipped(state::account_snapshot())) {
        emit("ev=ability_audit stage=helmet_wait result=timeout");
    }
    const state::AccountState account = state::account_snapshot();
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=ability_audit stage=begin chars=%zu", account.characterCount);
    if (written > 0) {
        emit({line.data(), static_cast<std::size_t>(written)});
    }
    for (std::size_t character = 0; character < account.characterCount; ++character) {
        report_character(character, account.characters[character]);
    }
    report_exotic_helmets();
    emit("ev=ability_audit stage=done");
    return 0;
}

} // namespace

/** Schedules the ability data audit when the settings ask for one. */
bool start_ability_audit() noexcept {
    const core::settings::client::Settings& settings = core::settings::get().client;
    if (!settings.abilityAudit) {
        return false;
    }
    const HANDLE thread = CreateThread(nullptr, 0, &audit_thread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        emit("ev=ability_audit stage=begin result=thread_fail");
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace sunrise::client::diagnostics