#pragma once

namespace sunrise::client::content::diagnostics {

/** Dumps the bucket, progression and per-bucket item tables once, for offline identification. */
void dump_investment_tables() noexcept;

/** Dumps every armor definition's ordinary-socket lanes and each lane's plug pool, once. */
void dump_armor_sockets() noexcept;

/** Dumps every mod-like socket lane's distinct plug pools, grouped by (bucket, role, lane), once. */
void dump_mod_lane_pools_once() noexcept;

} // namespace sunrise::client::content::diagnostics
