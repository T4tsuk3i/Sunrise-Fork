#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Parses Client-owned configuration over deterministic defaults. */
bool Parser::client_settings(client::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    client::Settings candidate = output;
    bool hasUserInterface = false;
    bool hasExternalServer = false;
    bool hasFadeRelease = false;
    bool hasForceJoinRequestReady = false;
    bool hasRegionPrivate = false;
    bool hasPinReplicatedRecord = false;
    bool hasHoldSpawn = false;
    bool hasSpawnHoldMs = false;
    bool hasCharacterStatBonus = false;
    bool hasCharacterStatRow = false;
    bool hasCharacterStatFillValue = false;
    bool hasCharacterStatFillFirst = false;
    bool hasCharacterStatFillLast = false;
    bool hasStatScanDelay = false;
    bool hasStatScanWindow = false;
    bool hasStatScanValues = false;
    bool hasCharacterStatRowBonuses = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "ui") {
            if (hasUserInterface || !client_ui_settings(candidate.userInterface)) {
                return false;
            }
            hasUserInterface = true;
        } else if (key == "external_server") {
            if (hasExternalServer || !client_external_settings(candidate.externalServer)) {
                return false;
            }
            hasExternalServer = true;
        } else if (key == "fade_release") {
            if (hasFadeRelease || !boolean(candidate.fadeRelease)) {
                return false;
            }
            hasFadeRelease = true;
        } else if (key == "force_join_request_ready") {
            if (hasForceJoinRequestReady || !boolean(candidate.forceJoinRequestReady)) {
                return false;
            }
            hasForceJoinRequestReady = true;
        } else if (key == "region_private") {
            if (hasRegionPrivate || !boolean(candidate.regionPrivate)) {
                return false;
            }
            hasRegionPrivate = true;
        } else if (key == "pin_replicated_record") {
            if (hasPinReplicatedRecord || !boolean(candidate.pinReplicatedRecord)) {
                return false;
            }
            hasPinReplicatedRecord = true;
        } else if (key == "hold_spawn") {
            if (hasHoldSpawn || !boolean(candidate.holdSpawn)) {
                return false;
            }
            hasHoldSpawn = true;
        } else if (key == "spawn_hold_ms") {
            std::uint64_t value = 0;
            if (hasSpawnHoldMs || !unsigned_integer(value) || value == 0
                || value > client::kMaximumSpawnHoldMs) {
                return false;
            }
            candidate.spawnHoldMs = value;
            hasSpawnHoldMs = true;
        } else if (key == "character_stat_bonus") {
            std::int64_t value = 0;
            const auto limit = static_cast<std::int64_t>(client::kMaximumCharacterStatBonus);
            if (hasCharacterStatBonus || !signed_integer(value) || value > limit
                || value < -limit) {
                return false;
            }
            candidate.characterStatBonus = static_cast<std::int32_t>(value);
            hasCharacterStatBonus = true;
        } else if (key == "character_stat_fill_value") {
            std::int64_t value = 0;
            const auto limit = static_cast<std::int64_t>(client::kMaximumCharacterStatBonus);
            if (hasCharacterStatFillValue || !signed_integer(value) || value > limit
                || value < -limit) {
                return false;
            }
            candidate.characterStatFillValue = static_cast<std::int32_t>(value);
            hasCharacterStatFillValue = true;
        } else if (key == "character_stat_fill_first") {
            std::int64_t value = 0;
            if (hasCharacterStatFillFirst || !signed_integer(value) || value < 0 || value > 127) {
                return false;
            }
            candidate.characterStatFillFirst = static_cast<std::int32_t>(value);
            hasCharacterStatFillFirst = true;
        } else if (key == "character_stat_fill_last") {
            std::int64_t value = 0;
            if (hasCharacterStatFillLast || !signed_integer(value) || value < 0 || value > 127) {
                return false;
            }
            candidate.characterStatFillLast = static_cast<std::int32_t>(value);
            hasCharacterStatFillLast = true;
        } else if (key == "stat_scan_delay_ms") {
            std::uint64_t value = 0;
            if (hasStatScanDelay || !unsigned_integer(value)) {
                return false;
            }
            candidate.statScanDelayMs = value;
            hasStatScanDelay = true;
        } else if (key == "stat_scan_window_bytes") {
            std::uint64_t value = 0;
            if (hasStatScanWindow || !unsigned_integer(value) || value == 0
                || value > client::kMaximumStatScanWindow) {
                return false;
            }
            candidate.statScanWindowBytes = value;
            hasStatScanWindow = true;
        } else if (key == "stat_scan_values") {
            if (hasStatScanValues || !consume('[')) {
                return false;
            }
            candidate.statScanValueCount = 0;
            if (!consume(']')) {
                for (;;) {
                    std::int64_t value = 0;
                    if (candidate.statScanValueCount >= candidate.statScanValues.size()
                        || !signed_integer(value)) {
                        return false;
                    }
                    candidate.statScanValues[candidate.statScanValueCount] =
                        static_cast<std::int32_t>(value);
                    ++candidate.statScanValueCount;
                    if (consume(']')) {
                        break;
                    }
                    if (!consume(',')) {
                        return false;
                    }
                }
            }
            hasStatScanValues = true;
        } else if (key == "character_stat_row") {
            std::int64_t value = 0;
            if (hasCharacterStatRow || !signed_integer(value) || value > 255 || value < -1) {
                return false;
            }
            candidate.characterStatRow = static_cast<std::int32_t>(value);
            hasCharacterStatRow = true;
        } else if (key == "character_stat_bonus_rows") {
            const auto limit = static_cast<std::int64_t>(client::kMaximumCharacterStatBonus);
            if (hasCharacterStatRowBonuses || !consume('[')) {
                return false;
            }
            if (consume(']')) {
                hasCharacterStatRowBonuses = true;
            } else {
                for (std::size_t index = 0; index < candidate.characterStatRowBonuses.size();
                     ++index) {
                    std::int64_t value = 0;
                    if (!signed_integer(value) || value > limit || value < -limit) {
                        return false;
                    }
                    candidate.characterStatRowBonuses[index] = static_cast<std::int32_t>(value);
                    if (index + 1 < candidate.characterStatRowBonuses.size() && !consume(',')) {
                        return false;
                    }
                }
                if (!consume(']')) {
                    return false;
                }
                hasCharacterStatRowBonuses = true;
            }
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
