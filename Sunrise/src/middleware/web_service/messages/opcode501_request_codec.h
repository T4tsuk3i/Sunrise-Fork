#pragma once

#include <cstddef>
#include <cstdint>

#include "../web_service_envelope.h"
#include "opcode501_codec.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** The create-character request: the identity the player picked on the creation screen. */
struct Request {
    std::uint8_t characterClass{};
    std::uint8_t gender{};
    std::uint8_t race{};
};

/**
 * Parses the create-character request body.
 *
 * The payload is a big-endian bitstream, not a byte-aligned struct: every field begins at
 * whatever bit offset the previous one ended on. Wire order is depth-first over
 * `{ int8 triple[3]; CharHeader header; char name[64] }`, where `triple` is race, gender and
 * class in that order and each entry is a 1-bit presence flag followed by 8 value bits biased
 * by +0x80. Only the triple is read here; the appearance header and the 64-byte name that
 * follow it are left untouched.
 *
 * @param message Parsed Web Service envelope.
 * @param request Receives the parsed creation fields.
 * @return True when the payload holds the identity triple, every presence flag is set, and
 *         race, gender and class are each in range.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
