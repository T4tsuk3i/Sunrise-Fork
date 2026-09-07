#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../ui/runtime/settings.h"
#include "external/definition.h"

namespace sunrise::core::settings::client {

/** Number of character stat rows the sheet names, excluding the light row. */
inline constexpr std::size_t kCharacterStatRowCount = 6;

/** A load this long has stopped making progress, so the spawn stops waiting for it. */
inline constexpr std::uint64_t kDefaultSpawnHoldMs = 30'000;
/** A load past this is a hang, not a slow machine, and holding the spawn would never end. */
inline constexpr std::uint64_t kMaximumSpawnHoldMs = 600'000;
/**
 * Highest flat character stat bonus the record will carry.
 * Far past anything the sheet was drawn for, which is the point: the ceiling exists so a typo
 * cannot assert a stat the client has no table row for.
 */
inline constexpr std::uint64_t kMaximumCharacterStatBonus = 1'000;

/** Values one stat-block scan may look for. */
inline constexpr std::size_t kStatScanValueCapacity = 8;
/** Widest window a scan searches for the remaining values, in bytes. */
inline constexpr std::uint64_t kMaximumStatScanWindow = 4096;

/** Read-only Client settings parsed by Core. */
struct Settings {
    /** In-game UI visibility and input policy. */
    ui::runtime::Settings userInterface;
    /** Points the Client at a server outside this process. Off answers everything in process. */
    external::Settings externalServer;
    /** Replaces stock bootflow textures that have matching DDS assets embedded in Sunrise. */
    bool customBootflowTextures{true};
    /** Moves the four Arrivals leg mods into the leg plug set, so the leg mod menu lists them. */
    bool socketMenuRouting{false};
    /**
     * Clears the visibility gates on the loaded lore presentation nodes.
     * On by default; a client stand-in until the unlock banks carry every gate the nodes read.
     */
    bool revealLoreBooks{true};
    /**
     * Reports a public region as private to the region transition.
     * On, a public region loads solo. Off, it waits for a public activity host, which is the
     * route to the citizen join. A forced destination loads solo either way.
     */
    bool regionPrivate{false};
    /**
     * Answers the orbit destination hold as released without calling the game's predicate.
     * The predicate waits for an armed destination or a starting cinematic, so skipping it
     * suppresses the orbit-side entry cinematic.
     */
    bool skipOrbitCinematicWait{false};
    /**
     * Pins the participation record to the replicated snapshot at `comp + 496`.
     * The msg-5 spawn hold reaches no other record.
     */
    bool pinReplicatedRecord{true};
    /**
     * Runs the player spawn after the world-transition fade is armed.
     * A spawn before the arm releases nothing, so the screen stays black. Settable because it is
     * the only thing that can turn an allowed spawn into a refusal.
     */
    bool holdSpawn{true};
    /** How long the spawn waits for a load. `hold_spawn` decides whether it waits at all. */
    std::uint64_t spawnHoldMs{kDefaultSpawnHoldMs};
    /**
     * Flat value added to each of the six character stat rows before the record is encoded.
     * The record carries a signed stat and nothing on this side clamps it, so this is how far
     * past the native ceiling a stat can be asserted. Zero leaves every stat at its rolled total.
     */
    std::int32_t characterStatBonus{};
    /**
     * Stat row the bonus applies to, or negative for every character row.
     * Targeting one row keeps the other five at their rolled totals, so a measurement cannot be
     * confounded by five stats moving at once.
     */
    std::int32_t characterStatRow{-1};
    /**
     * Value written to every character stat row the constants do not name, or zero for none.
     * The six named rows are the ones the sheet shows; an ability that reads some other row would
     * be invisible to every experiment that only moves those six, so this fills the rest.
     */
    std::int32_t characterStatFillValue{};
    /** First row the fill covers. */
    std::int32_t characterStatFillFirst{};
    /** Last row the fill covers, bounded by the record's own stat table. */
    std::int32_t characterStatFillLast{31};
    /**
     * Delay before the one-shot stat-block scan runs, or zero to leave it off.
     * The scan wants the sandbox live, so it runs long after boot rather than at activation.
     */
    std::uint64_t statScanDelayMs{};
    /** Bytes searched for the remaining values once the first one matches. */
    std::uint64_t statScanWindowBytes{64};
    /** Values the scan looks for, in order. */
    std::array<std::int32_t, kStatScanValueCapacity> statScanValues{};
    /** Filled leading entries in `statScanValues`. */
    std::size_t statScanValueCount{};
    /**
     * One flat bonus per character stat row, in the build-data rows' ascending order.
     * Distinct values make the sheet name its own rows in one load: reading the six displayed
     * numbers against the probe's `r<key>=<value>` line pins each row key to its stat. When any
     * entry is nonzero it replaces the single-bonus setting above for the six rows; an all-zero
     * array falls back to `character_stat_bonus` / `character_stat_row`.
     */
    std::array<std::int32_t, kCharacterStatRowCount> characterStatRowBonuses{};
    /**
     * Dumps the installed build's bucket, progression and armor-socket tables to the log, once.
     * Runs on every investment refresh with no gate of its own otherwise, so this exists to keep
     * it off outside a session that actually wants the dump. Off by default.
     */
    bool investmentDump{false};
};

} // namespace sunrise::core::settings::client
