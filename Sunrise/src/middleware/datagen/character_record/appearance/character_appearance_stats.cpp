#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../state/build_data/armor/stats/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/equipment/light/definition.h"
#include "../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::middleware::datagen::character_record::appearance {
namespace {

namespace constants = state::build_data::constants;
namespace armor = state::build_data::armor;

/**
 * Sums one definition's declared contribution to a single stat row.
 * @param definitionIndex Native item or plug index.
 * @param row Stat table row.
 * @return The declared value, or 0 when the definition declares none.
 */
[[nodiscard]] std::int32_t definition_total(std::uint16_t definitionIndex,
                                            std::uint8_t row) noexcept {
    details::Definition detail{};
    if (definitionIndex == details::kUnavailableItemIndex
        || !state::build_data::find_configured_item_detail(definitionIndex, detail)) {
        return 0;
    }
    std::int32_t total = 0;
    const std::size_t stats =
        detail.statCount < detail.stats.size() ? detail.statCount : detail.stats.size();
    for (std::size_t entry = 0; entry < stats; ++entry) {
        if (detail.stats[entry].row == row) {
            total += detail.stats[entry].value;
        }
    }
    return total;
}

/**
 * Sums one equipped item's own roll plus every plug its sockets hold.
 * An armour piece keeps only a token value on its own definition, so counting the definition alone
 * understates it by its whole roll.
 * @param equipped Effective plug lanes.
 * @param row Stat table row.
 * @return The item's total for that row.
 */
[[nodiscard]] std::int32_t item_total(const Equipped& equipped, std::uint8_t row) noexcept {
    std::int32_t total = definition_total(equipped.definitionIndex, row);
    for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
        total += definition_total(resolve_effective_plug(equipped, lane), row);
    }
    return total;
}

/**
 * Collects every stat row one equipped item or its plugs declare.
 * @param equipped Effective plug lanes.
 * @param count Occupied entries, advanced per distinct row.
 * @return False for an invalid stat row or insufficient storage.
 */
[[nodiscard]] bool
collect_rows(const Equipped& equipped, std::span<std::uint8_t> rows, std::size_t& count) noexcept {
    const std::size_t lanes = equipped.laneCount + 1;
    for (std::size_t source = 0; source < lanes; ++source) {
        const std::uint16_t definitionIndex =
            source == 0 ? equipped.definitionIndex : resolve_effective_plug(equipped, source - 1);
        details::Definition detail{};
        if (definitionIndex == details::kUnavailableItemIndex
            || !state::build_data::find_configured_item_detail(definitionIndex, detail)) {
            continue;
        }
        const std::size_t stats =
            detail.statCount < detail.stats.size() ? detail.statCount : detail.stats.size();
        for (std::size_t entry = 0; entry < stats; ++entry) {
            const std::uint8_t row = detail.stats[entry].row;
            if (row == details::kEmptyStatRow) {
                continue;
            }
            if (row >= constants::kStatRowCount) {
                return false;
            }
            if (std::find(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(count), row)
                == rows.begin() + static_cast<std::ptrdiff_t>(count)) {
                if (count >= rows.size()) {
                    return false;
                }
                rows[count++] = row;
            }
        }
    }
    return true;
}

/**
 * Appends one row to a stat table.
 * The client writes only rows that contribute, so a row worth 0 is omitted rather than written as
 * a live key asserting the stat is exactly 0.
 * @param row Stat table row.
 * @param value Signed total.
 * @param count Occupied rows, advanced when the row is written.
 */
void append(std::uint8_t row,
            std::int32_t value,
            std::array<layout::StatRow, layout::kStatRowCapacity>& table,
            std::size_t& count) noexcept {
    if (value <= 0 || count >= table.size()) {
        return;
    }
    table[count].key = static_cast<std::int8_t>(row);
    table[count].value = value;
    ++count;
}

/** Writes item Power and definition stats, rejecting invalid rows or insufficient space. */
[[nodiscard]] bool
apply_weapon_table(const Equipped& equipped,
                   std::uint8_t powerRow,
                   std::int32_t power,
                   std::array<layout::StatRow, layout::kStatRowCapacity>& table) noexcept {
    std::array<std::uint8_t, constants::kStatRowCount> rows{powerRow};
    std::size_t rowCount = 1;
    if (!collect_rows(equipped, rows, rowCount)) {
        return false;
    }
    std::sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(rowCount));
    std::array<layout::StatRow, layout::kStatRowCapacity> staged{};
    std::size_t written = 0;
    for (std::size_t entry = 0; entry < rowCount; ++entry) {
        const std::uint8_t row = rows[entry];
        const std::int32_t value = row == powerRow ? power : item_total(equipped, row);
        if (value > 0 && written >= staged.size()) {
            return false;
        }
        append(row, value, staged, written);
    }
    table = staged;
    return true;
}

/** @param table Encoded rows. @param count Occupied rows. @return True when the row is written. */
[[nodiscard]] bool row_written(const std::array<layout::StatRow, layout::kStatRowCapacity>& table,
                              std::size_t count,
                              std::int32_t row) noexcept {
    for (std::size_t entry = 0; entry < count; ++entry) {
        if (static_cast<std::int32_t>(table[entry].key) == row) {
            return true;
        }
    }
    return false;
}

/**
 * Writes one value to every stat row in range the named rows left empty.
 * An ability whose cooldown reads a row the investment constants never name is invisible to any
 * experiment that only moves the six the sheet shows; filling the remainder makes such a row
 * announce itself by changing a cooldown, and the fill range then bisects to find which one.
 * @param settings Client tuning carrying the fill value and its bounds.
 * @param table Character stat table receiving the rows.
 * @param count Occupied rows, advanced per written row.
 * @return Rows the fill actually wrote.
 */
std::size_t fill_unnamed_rows(const core::settings::client::Settings& settings,
                              std::array<layout::StatRow, layout::kStatRowCapacity>& table,
                              std::size_t& count) noexcept {
    if (settings.characterStatFillValue == 0
        || settings.characterStatFillLast < settings.characterStatFillFirst) {
        return 0;
    }
    std::size_t filled = 0;
    for (std::int32_t row = settings.characterStatFillFirst;
         row <= settings.characterStatFillLast && count < table.size();
         ++row) {
        if (row_written(table, count, row)) {
            continue;
        }
        append(static_cast<std::uint8_t>(row), settings.characterStatFillValue, table, count);
        ++filled;
    }
    return filled;
}

/** Diagnostic latch: the bonus is fixed for a run, so the first encoded table settles it. */
std::atomic<bool> g_reportedProbe{};

/**
 * Reports the character stat table exactly as it was encoded, once per run.
 * The sheet clamps what it draws, so a bonus that renders as the native ceiling is ambiguous on
 * its own; this line is the value that was actually asserted, whatever the sheet then shows.
 * @param bonus Configured flat bonus. Zero reports nothing.
 * @param table Encoded character stat table.
 * @param count Occupied rows.
 */
void report_probe(std::uint64_t soid,
                  std::int32_t bonus,
                  const std::array<layout::StatRow, layout::kStatRowCapacity>& table,
                  std::size_t count) noexcept {
    const core::settings::client::Settings& tuning = core::settings::get().client;
    bool hasRowBonuses = false;
    for (const std::int32_t value : tuning.characterStatRowBonuses) {
        hasRowBonuses = hasRowBonuses || value != 0;
    }
    if ((bonus == 0 && !hasRowBonuses) || g_reportedProbe.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::array<char, 256> line{};
    const int header = std::snprintf(
        line.data(),
        line.size(),
        "ev=char_stats stage=probe soid=0x%llX bonus=%d rows=%zu",
        static_cast<unsigned long long>(soid),
        bonus,
        count);
    if (header <= 0) {
        return;
    }
    auto used = static_cast<std::size_t>(header);
    for (std::size_t entry = 0; entry < count && used + 24 < line.size(); ++entry) {
        const int extra = std::snprintf(line.data() + used,
                                        line.size() - used,
                                        " r%d=%d",
                                        static_cast<int>(table[entry].key),
                                        table[entry].value);
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
    core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), used});
}

/** Diagnostic latch: the equipped set is fixed for one encode, so the first one settles it. */
std::atomic<bool> g_reportedItems{};

/**
 * Reports each equipped item's plugs and its contribution to the six character rows, once.
 * The sheet's own number cannot be matched to a row without this: it names which row the grenade
 * mods actually feed, and shows whether those plugs are being summed at all.
 * @param instances Resolved equipped set.
 * @param rows The six named character stat rows.
 */
void report_item_stats(std::uint64_t soid,
                       const family4::loadout::ResolvedInstances& instances,
                       const std::array<std::uint8_t, constants::kCharacterStatRowCount>& rows) noexcept {
    if (g_reportedItems.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        details::Definition detail{};
        Equipped equipped{};
        if (!resolve_equipped(instances.items[index], detail, equipped)) {
            continue;
        }
        std::array<char, 384> line{};
        const int header = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=char_stats stage=item soid=0x%llX slot=%u def=%u "
                                         "hash=0x%08X lanes=%zu",
                                         static_cast<unsigned long long>(soid),
                                         static_cast<unsigned>(equipped.equipmentSlot),
                                         static_cast<unsigned>(equipped.definitionIndex),
                                         detail.definitionHash,
                                         equipped.laneCount);
        if (header <= 0) {
            continue;
        }
        auto used = static_cast<std::size_t>(header);
        const family4::instance::ArmorMeta& armor = instances.items[index].instance.armor;
        const int armorHeader = std::snprintf(line.data() + used,
                                              line.size() - used,
                                              " a=%u t=%u m=%u s=%llx",
                                              static_cast<unsigned>(armor.archetype),
                                              static_cast<unsigned>(armor.gearTier),
                                              static_cast<unsigned>(armor.masterworkLevel),
                                              static_cast<unsigned long long>(armor.setHash));
        if (armorHeader > 0) {
            used += static_cast<std::size_t>(armorHeader);
        }
        for (std::size_t lane = 0; lane < equipped.laneCount && used + 24 < line.size(); ++lane) {
            // The definition's own initial plug sits beside the account's, because a lane the
            // account left empty while the definition names one is a seeding failure, not gear.
            const std::uint16_t initial = lane < detail.initialPlugIndices.size()
                                              ? detail.initialPlugIndices[lane]
                                              : details::kUnavailableItemIndex;
            const int extra = std::snprintf(line.data() + used,
                                            line.size() - used,
                                            " p%zu=%u/%u",
                                            lane,
                                            static_cast<unsigned>(equipped.plugs[lane]),
                                            static_cast<unsigned>(initial));
            if (extra > 0) {
                used += static_cast<std::size_t>(extra);
            }
        }
        for (const std::uint8_t row : rows) {
            const std::int32_t total = item_total(equipped, row);
            if (total == 0 || used + 24 >= line.size()) {
                continue;
            }
            const int extra = std::snprintf(line.data() + used,
                                            line.size() - used,
                                            " r%u=%d",
                                            static_cast<unsigned>(row),
                                            total);
            if (extra > 0) {
                used += static_cast<std::size_t>(extra);
            }
        }
        core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), used});
    }
}

/** Diagnostic latch: the constants are a boot-time domain, so one line settles their absence. */
std::atomic<bool> g_reportedMissingConstants{};

/** Reports once that the installed investment constants are unavailable or invalid. */
void report_missing_constants() noexcept {
    if (g_reportedMissingConstants.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     "ev=char_stats stage=constants result=unavailable_or_invalid");
}

} // namespace

/** Diagnostic latch: one effective-stat report settles it for a run. */
std::atomic<bool> g_reportedArmor3{};

/**
 * Reports the centralized Armor 3.0 effective stat table, once per run, under the modern names.
 * This is the authoritative result of the pipeline; the sheet's legacy-row numbers should match
 * the legacy totals these modern values translated from.
 */
void report_armor3_effective(
    const std::array<std::int32_t, armor::stats::kStatCount>& effective,
    const std::array<std::uint8_t, constants::kCharacterStatRowCount>& rows,
    const std::array<armor::stats::Definition, armor::stats::kStatCount>& definitions) noexcept {
    if (g_reportedArmor3.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::array<char, 320> line{};
    int header = std::snprintf(line.data(), line.size(), "ev=armor3 stage=effective count=%zu",
                               armor::stats::kStatCount);
    if (header <= 0) {
        return;
    }
    auto used = static_cast<std::size_t>(header);
    for (std::size_t index = 0; index < armor::stats::kStatCount && used + 32 < line.size();
         ++index) {
        const armor::stats::Definition& definition = definitions[index];
        const int extra =
            std::snprintf(line.data() + used,
                          line.size() - used,
                          " %s=%d(row[%u]=%u)",
                          definition.displayName,
                          static_cast<int>(effective[index]),
                          static_cast<unsigned>(definition.legacyRowIndex),
                          static_cast<unsigned>(rows[definition.legacyRowIndex]));
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
    core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), used});
}

/** Fills the character stat table and the three per-weapon stat tables. */
bool apply_stats(const state::CharacterState& character,
                 const family4::loadout::ResolvedInstances& instances,
                 std::int32_t light,
                 layout::Appearance& appearance) noexcept {
    constants::InvestmentConstants named{};
    if (!state::build_data::find_investment_constants(named) || !constants::valid(named)) {
        // Publishing an absent or unusable weapon Power row silently selects the damage floor.
        report_missing_constants();
        return false;
    }
    std::array<std::uint8_t, constants::kCharacterStatRowCount> rows = named.characterStatRows;
    std::sort(rows.begin(), rows.end());

    // The light row is what the banner reads, so it stays the equipment's own number; only the
    // six character rows carry the Armor 3.0 pipeline.
    const core::settings::client::Settings& tuning = core::settings::get().client;
    const std::int32_t bonus = tuning.characterStatBonus;
    const std::int32_t bonusRow = tuning.characterStatRow;
    bool hasRowBonuses = false;
    for (const std::int32_t value : tuning.characterStatRowBonuses) {
        hasRowBonuses = hasRowBonuses || value != 0;
    }

    const armor::stats::Table& definitions = armor::stats::table();
    std::array<std::int32_t, armor::stats::kStatCount> effective{};
    std::size_t written = 0;
    append(named.lightStatRow, light, appearance.characterStats, written);

    // The seasonal Artifact contributes its stat row on top of the equipment, before the six
    // armour rows resolve their own totals.
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        details::Definition detail{};
        Equipped equipped{};
        if (!resolve_equipped(instances.items[index], detail, equipped)
            || detail.definitionHash != state::kSeasonalArtifactItemHash || detail.statCount == 0
            || detail.stats.front().row == details::kEmptyStatRow) {
            continue;
        }
        append(detail.stats.front().row,
               state::artifact_power_bonus(),
               appearance.characterStats,
               written);
        break;
    }

    std::size_t rowIndex = 0;
    for (const std::uint8_t row : rows) {
        std::int32_t total = 0;
        for (std::size_t index = 0; index < instances.itemCount; ++index) {
            details::Definition detail{};
            Equipped equipped{};
            if (resolve_equipped(instances.items[index], detail, equipped)) {
                total += item_total(equipped, row);
            }
        }
        std::int32_t effectiveBonus = 0;
        if (hasRowBonuses) {
            effectiveBonus = rowIndex < tuning.characterStatRowBonuses.size()
                                 ? tuning.characterStatRowBonuses[rowIndex]
                                 : 0;
        } else if (bonusRow < 0 || static_cast<std::int32_t>(row) == bonusRow) {
            effectiveBonus = bonus;
        }
        const std::int32_t legacyTotal = total + effectiveBonus;

        // Every modern stat that owns this legacy row receives the legacy total, then its config
        // baseline and a clamp to the 0-200 design ceiling. This is the single authoritative
        // calculation: nothing outside this module derives stat behaviour independently.
        for (std::size_t stat = 0; stat < armor::stats::kStatCount && rowIndex < rows.size();
             ++stat) {
            const armor::stats::Definition& definition = definitions.values[stat];
            if (definition.legacyRowIndex != rowIndex) {
                continue;
            }
            std::int32_t value = legacyTotal + (definition.baseline ? armor::stats::kBaseClazzStat
                                                                   : 0);
            value = value < armor::stats::kMaximumStat ? value : armor::stats::kMaximumStat;
            effective[stat] = value;
            append(row, value, appearance.characterStats, written);
        }
        ++rowIndex;
    }
    const std::size_t filled = fill_unnamed_rows(tuning, appearance.characterStats, written);
    // Character select encodes every character, so an unqualified latch reports whichever one is
    // encoded first rather than the one under test. Only the selected character is the subject.
    if (character.selected) {
        if (filled != 0) {
            std::array<char, 160> line{};
            const int n = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=char_stats stage=fill rows=%zu value=%d first=%d "
                                        "last=%d",
                                        filled,
                                        tuning.characterStatFillValue,
                                        tuning.characterStatFillFirst,
                                        tuning.characterStatFillLast);
            if (n > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(n)});
            }
        }
        report_probe(character.soid, bonus, appearance.characterStats, written);
        report_armor3_effective(effective, rows, definitions.values);
        report_item_stats(character.soid, instances, rows);
    }

    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        const std::uint8_t slot = instances.items[index].equipmentSlot;
        const auto weapon = static_cast<std::size_t>(slot - kFirstWeaponSlot);
        if (slot < kFirstWeaponSlot || weapon >= appearance.weaponStats.size()) {
            continue;
        }
        details::Definition detail{};
        Equipped equipped{};
        std::int32_t power = 0;
        if (!resolve_equipped(instances.items[index], detail, equipped)
            || !state::equipment::light::item_power(instances.items[index].instance.level, power)
            || !apply_weapon_table(
                equipped, named.weaponPowerStatRow, power, appearance.weaponStats[weapon])) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::datagen::character_record::appearance
