#pragma once

namespace sunrise::client::diagnostics {

/**
 * Starts the one-shot search for internal build cvar/flag name literals in live process memory.
 *
 * An unreleased build's own debug flags (invulnerability, noclip, and the like) are typically
 * registered under a short ASCII name that ships as a string literal in the image's read-only
 * data, plaintext in memory even though the file is encrypted on disk. This walks committed
 * readable memory for a fixed list of likely candidate names and reports where each one lands,
 * with the surrounding bytes printed as text: literal strings are usually pooled together by the
 * compiler, so a real hit's neighbours are worth reading by eye for names not on the list.
 *
 * This narrows where to look; it does not identify a working flag by itself. A string hit still
 * has to be traced to what reads it before it means anything.
 *
 * @return True when the scan was scheduled; false when it is disabled or the thread was refused.
 */
[[nodiscard]] bool start_debug_flag_scan() noexcept;

} // namespace sunrise::client::diagnostics
