#pragma once

namespace sunrise::client::diagnostics {

/**
 * Starts the one-shot search for the sandbox's own character stat block.
 *
 * The record Sunrise encodes carries six character stats, and the client's ability code honours
 * three of them: the class ability tracks Recovery exactly while grenade, melee and super compute
 * at zero forever. So somewhere in the process there is a block holding this character's stats in
 * which the honoured rows carry our values and the ignored rows do not. Finding that block names
 * what governs cooldowns; the encrypted image cannot be read from disk, but it is plaintext here.
 *
 * The scan runs on its own thread after a configured delay, so the player is in a world with the
 * sandbox live rather than on the character select screen, and reads memory without touching a
 * game thread.
 *
 * @return True when the scan was scheduled; false when it is disabled or the thread was refused.
 */
[[nodiscard]] bool start_stat_block_scan() noexcept;

} // namespace sunrise::client::diagnostics
