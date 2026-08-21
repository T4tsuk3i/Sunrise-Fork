#pragma once

#include <cstdint>

#include "../defaults/definition.h"

namespace sunrise::state::activity::cutscene {

/**
 * Routes the first load after a character is selected into that character's opening cutscene.
 *
 * A cutscene is an ordinary destination here: the installed packages carry them as `cine_*`
 * scenarios alongside every patrol and mission space, so playing one is a matter of sending the
 * player there rather than anywhere else. Homecoming has no mission node to launch, so the
 * cutscene is owed the moment the player picks a character, and the normal default destination
 * takes over once they ask to go somewhere themselves.
 */

/**
 * Owes the opening cutscene to the next load.
 * @param characterSoid Character that was selected, recorded for the log only.
 */
void arm(std::uint64_t characterSoid) noexcept;

/** Drops the debt, so the next load uses the authored default destination. */
void disarm() noexcept;

/** @return True while a cutscene is owed. */
[[nodiscard]] bool armed() noexcept;

/**
 * Replaces the default destination with the owed cutscene.
 *
 * The cutscene's bubble layout is read from the scenario catalog rather than authored, so only
 * its package name is fixed here and a destination the installed packages do not carry is left
 * alone instead of publishing a layout no map can satisfy.
 *
 * @param defaults Snapshot to rewrite in place.
 * @return True when a cutscene was owed and its destination replaced the default.
 */
[[nodiscard]] bool apply(defaults::ActivityDefaults& defaults) noexcept;

} // namespace sunrise::state::activity::cutscene
