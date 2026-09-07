#pragma once

#include "../../account/account_state.h"

namespace sunrise::state::runtime::persistence {

/** Layout version of the state file this build writes and expects. */
inline constexpr std::uint32_t kStateVersion = 1;

/**
 * Gap between background flush passes.
 * A mutation that lands between flushes is only in memory until the next pass, or the final
 * flush on shutdown, reaches it. This bound keeps that window short without writing the whole
 * account to disk on every single mutation.
 */
inline constexpr std::uint32_t kSaveIntervalMs = 500;

/**
 * Resolves the state file path beside the loaded module and starts the background flush thread.
 * Persistence stays soft-disabled, and `request_save` a no-op, when the path cannot be resolved;
 * a failure to start the flush thread itself falls back to `request_save` writing synchronously.
 * Either failure is reported, not raised, so a boot never fails here.
 * @param module Loaded DLL, used to find the artifact directory.
 * @return Always true; failures are soft and only ever reported.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/**
 * Loads the persisted state file over the initial account, falling back through its backups.
 *
 * The account seeded from settings.json is passed in and stays in place unless a document
 * clears every gate: it must parse, declare no layout version newer than this build knows, carry
 * the same `primarySoid` as the account it is being applied to, and produce a state that passes
 * `account::valid`. A file at an older or unversioned layout is accepted and run through the same
 * tolerant field-by-field parser new fields already use: an added field takes its struct default,
 * a removed one is skipped as an unrecognized key. That only covers a purely additive or
 * subtractive change; a version that changes what an existing field means still needs a real
 * migration, the way settings.json's upgrade table does.
 *
 * state.json is tried first, and only state.json — never a source this build wrote to. On any
 * failure there, `.bak` is tried through the identical gauntlet, then `.bak2`. This exists so a
 * bad primary file cannot end up silently replacing a `.bak` that still held the real account:
 * without it, the first mutation after a failed load would write the freshly-seeded (effectively
 * empty) account over `.bak`, and the genuine save would be gone within a couple of ordinary
 * saves. A source that recovers the account is reported loudly, since it means real progress is
 * missing relative to what state.json itself last held. Every failure along the way is reported
 * too, except a source that was never written yet, which is not a failure.
 *
 * @param output Seeded with the authored account; receives the persisted one when it is accepted.
 * @return True when the boot may continue, which is every case; failures are reported, not raised.
 */
[[nodiscard]] bool load(state::AccountState& output) noexcept;

/**
 * Marks the account dirty for the next background flush pass and returns immediately.
 * The write itself happens off the caller's thread within `kSaveIntervalMs`, or at the latest
 * during `shutdown`. Falls back to an immediate synchronous write if the flush thread never
 * started, so a mutation is never silently lost.
 * @return False when the state file path never resolved (nowhere to write), or, on the fallback
 * path with no flush thread running, when that immediate write itself failed.
 */
[[nodiscard]] bool request_save() noexcept;

/**
 * Stops the flush thread, writing one last time if a mutation is still unsaved, then drops the
 * resolved file path.
 */
void shutdown() noexcept;

} // namespace sunrise::state::runtime::persistence
