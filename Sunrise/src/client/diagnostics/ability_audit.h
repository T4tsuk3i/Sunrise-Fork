#pragma once

namespace sunrise::client::diagnostics {

/**
 * Dumps the published game-data ability tables once the content slice is ready.
 *
 * The class-ability alternative semantics (Gambler's vs Marksman's Dodge), the super variants,
 * and the equipped exotic helmet all ship as content: the socket entry lists, their pool-based
 * ability buckets, and the item definitions. This audit reads those already-publicated tables and
 * logs them instead of probing the sandbox for stat values, so the authored definitions each
 * ability hash names (their tiers, stat rows, socket lists and sandbox perks) are on the record.
 *
 * It runs on its own thread and waits until the item, socket-entry-list, and ability-bucket
 * domains are published, then dumps and stops. Disabled unless settings request it.
 *
 * @return True when the audit was scheduled; false when it is disabled or the thread was refused.
 */
[[nodiscard]] bool start_ability_audit() noexcept;

} // namespace sunrise::client::diagnostics