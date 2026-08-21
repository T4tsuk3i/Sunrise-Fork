#pragma once

#include "../../account/account_state.h"

namespace sunrise::state::runtime::persistence {

/** Layout version of the state file this build writes and expects. */
inline constexpr std::uint32_t kStateVersion = 1;

/**
 * Resolves the state file path beside the loaded module.
 * @param module Loaded DLL, used to find the artifact directory.
 * @return True when the path is resolved.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/**
 * Loads the persisted state file over the initial account.
 *
 * The account seeded from settings.json is passed in and stays in place unless the document
 * clears every gate: it must parse, declare either this layout version or none at all, carry the
 * same `primarySoid` as the account it is being applied to, and produce a state that passes
 * `account::valid`. Anything else is reported and skipped, so a damaged or foreign save costs the
 * session's changes rather than the boot.
 *
 * @param output Seeded with the authored account; receives the persisted one when it is accepted.
 * @return True when the boot may continue, which is every case; failures are reported, not raised.
 */
[[nodiscard]] bool load(state::AccountState& output) noexcept;

/**
 * Saves the current account state to the state file atomically.
 * The caller must hold no locks; this function acquires g_stateLock internally.
 * @return True when the state was written successfully.
 */
[[nodiscard]] bool save() noexcept;

/** Drops the resolved file path. */
void shutdown() noexcept;

} // namespace sunrise::state::runtime::persistence
