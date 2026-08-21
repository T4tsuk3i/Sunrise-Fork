#pragma once

#include <cstdint>

#include "../account/account_state.h"

namespace sunrise::state {

/**
 * Creates a new character from a create-character request.
 * @param account In-out parameter; receives the appended character on success.
 * @param characterClass Character class (0=titan, 1=hunter, 2=warlock).
 * @param gender Character gender (0=male, 1=female).
 * @param race Character race (0=human, 1=awoken, 2=exo).
 * @param characterSoid Receives the SOID of the newly created character.
 * @return True when the character was created and the account validates.
 */
[[nodiscard]] bool create_character(AccountState& account,
                                    std::uint8_t characterClass,
                                    std::uint8_t gender,
                                    std::uint8_t race,
                                    std::uint64_t& characterSoid) noexcept;

/**
 * Deletes a character by SOID.
 * @param account In-out parameter; receives the modified account on success.
 * @param characterSoid SOID of the character to delete.
 * @return True when the character was found and removed, and the account validates.
 */
[[nodiscard]] bool delete_character(AccountState& account, std::uint64_t characterSoid) noexcept;

} // namespace sunrise::state
