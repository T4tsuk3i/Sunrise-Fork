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
/** Hashes the slot sweep may search each table's bytes for. */
inline constexpr std::size_t kHashSearchCapacity = 8;
/** Most bytes the slot sweep will hex-dump from one table. */
inline constexpr std::uint64_t kMaximumSlotSweepBytes = 1024;
/** Widest window a scan searches for the remaining values, in bytes. */
inline constexpr std::uint64_t kMaximumStatScanWindow = 4096;
/**
 * Longest gap a repeating scan waits between passes, in milliseconds.
 * A single pass over process memory takes several seconds, so nothing under that is meaningful;
 * this bound is generous headroom past it, not a tuned floor.
 */
inline constexpr std::uint64_t kMaximumStatScanIntervalMs = 60'000;
/**
 * Passes a repeating scan runs before it stops, so a forgotten setting cannot scan forever.
 * At the shortest allowed interval this is several hours of continuous passes, comfortably past
 * any one play session, so it is a safety ceiling rather than something normal use reaches.
 */
inline constexpr std::size_t kMaximumStatScanPasses = 800;

/** Read-only Client settings parsed by Core. */
struct Settings {
    /** In-game UI visibility and input policy. */
    ui::runtime::Settings userInterface;
    /** Points the Client at a server outside this process. Off answers everything in process. */
    external::Settings externalServer;
    /**
     * Releases the world-transition fade channel at the in-world step.
     * The client only releases it on the player spawn, so this covers a spawn that never runs
     * and leaves the world black. On by default.
     */
    bool fadeRelease{true};
    /**
     * Forces the activity session's status 5-to-6 ready check.
     * Two of its five terms are client flags no host message reaches, so the host cannot open it.
     */
    bool forceJoinRequestReady{true};
    /**
     * Reports a public region as private to the region transition.
     * On, a public region loads solo. Off, it waits for a public activity host, which is the
     * route to the citizen join. A forced destination loads solo either way.
     */
    bool regionPrivate{false};
    /**
     * Pins the participation record to the replicated snapshot at `comp + 496`.
     * Off, the record is the local one at `comp + 1256`, whose spawn-gate byte no wire field
     * reaches.
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
     * Delay before the first stat-block scan pass, zero to start at hook activation.
     * Whether the scan runs at all is decided by `stat_scan_values` being non-empty, not by this
     * field: a scan that only starts once the sandbox is already live can miss the window it
     * exists to catch, so the honest default is to cover the boot from as close to the start as
     * possible rather than guess how long loading takes.
     */
    std::uint64_t statScanDelayMs{};
    /** Bytes searched for the remaining values once the first one matches. */
    std::uint64_t statScanWindowBytes{64};
    /**
     * Gap between scan passes, or zero to run the single configured pass and stop.
     * The block that carries a stat is one snapshot, but the code that reads it only touches the
     * stack during the moment an ability computes something from it, and a one-shot scan has no
     * way to land on that moment. Repeating the pass across ordinary play catches it without
     * requiring the delay to be timed against a button press.
     */
    std::uint64_t statScanIntervalMs{};
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
     * Reports every sandbox perk the equipped set contributes, and the bank the encoder shipped.
     * An exotic's effect is a sandbox perk carried by the item or one of its plugs, so a perk that
     * changes nothing in play has two possible causes: the record never carried it, or the client
     * ignored it. The census separates them, which no in-game observation can. Off by default,
     * because it prints one line per equipped item on the first encode of every boot.
     */
    bool perkCensus{false};
    /**
     * Dumps the investment constants bytes around the character stat rows.
     * Seven scalars are read out of that blob at offsets recovered from the client, and the rest
     * of it has never been looked at. The six stat rows sit in two separate runs with a gap
     * between them, which is the same three-and-three split the working and non-working stats
     * fall into, so what occupies that gap is worth seeing. Off by default; one block per boot.
     */
    bool constantsDump{false};
    /**
     * Reports every investment root slot, naming the table each one holds.
     * Eight slots are addressed by name and the rest of the root has never been enumerated, so a
     * table this build carries is invisible until something asks for it. The stat definition table
     * is the one being looked for, since nothing else maps a stat row to the identity the client
     * knows it by. Off by default; it reads every slot's blob, which lengthens one boot.
     */
    bool slotSweep{false};
    /**
     * Values the slot sweep searches each table's bytes for, as 32-bit little-endian words.
     * A definition is identified across builds by its hash, and nothing this server extracts
     * carries one for a stat, so the table holding them can only be found by looking for a hash
     * whose value is already known. Authored here rather than in source because the values are
     * game data, and this keeps them in configuration where they belong. Searched only when the
     * slot sweep runs.
     */
    std::array<std::uint32_t, kHashSearchCapacity> hashSearchValues{};
    /** Filled leading entries in `hashSearchValues`. */
    std::size_t hashSearchCount{};
    /**
     * Leading bytes of each table the slot sweep hex-dumps, or zero to dump none.
     * The sweep only runs on a boot that re-extracts, which is slow, so this exists to take the
     * evidence for later analysis while that boot is happening: a table's row stride and the
     * position of a hash inside a row are both readable from its opening bytes.
     */
    std::uint64_t slotSweepBytes{};
    /**
     * One slot whose table is dumped in full, or negative for none.
     * The per-slot byte cap keeps a sweep readable, but a table being studied is wanted whole, and
     * the sweep only runs on a boot that re-extracts. Naming one slot takes all of it in the same
     * pass, so the evidence outlives the boot that produced it.
     */
    std::int32_t slotDumpIndex{-1};
};

} // namespace sunrise::core::settings::client
