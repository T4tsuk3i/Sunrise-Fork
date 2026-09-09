#pragma once

namespace sunrise::client::diagnostics {

/**
 * Starts a one-shot search for the character stat block, then polls it for the rest of the
 * session and logs whenever any of its six floats changes.
 *
 * Locates the block the same way `stat_block_scan` does (searching for `stat_scan_values` as a
 * packed float sequence), but instead of a single dump this keeps a background thread reading it
 * on an interval for as long as the process runs. A hardware breakpoint would name the exact
 * reading instruction, but VMProtect likely checks the debug registers as part of its own
 * anti-tamper, so arming one risks the session being detected and killed. A plain periodic read
 * is invisible to any such check: it is nothing more than this module dereferencing a pointer,
 * the same as any other code in the process. What it loses is the caller's identity; what it
 * keeps is safety, and it still answers the question that matters here -- whether anything writes
 * to a given stat's slot during real play, and roughly when.
 *
 * @return True when watching was scheduled; false when it is disabled or the thread was refused.
 */
[[nodiscard]] bool start_stat_block_watch() noexcept;

} // namespace sunrise::client::diagnostics
