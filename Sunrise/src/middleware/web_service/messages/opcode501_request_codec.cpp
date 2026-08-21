#include "opcode501_request_codec.h"

#include <span>

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** Three presence flags and three biased values: the identity triple ends at this bit. */
constexpr std::size_t kIdentityBits = 27;

/** Values wider than this are never read here. */
constexpr std::size_t kMaxFieldBits = 32;

/** Every 8-bit value on the wire carries this bias. */
constexpr std::uint32_t kByteBias = 0x80U;

/** Highest valid race, gender and class wire values, matching the state enums. */
constexpr std::uint8_t kMaxRace = 2;
constexpr std::uint8_t kMaxGender = 1;
constexpr std::uint8_t kMaxClass = 2;

/** Reads big-endian bit fields out of a payload that honours no byte alignment. */
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) noexcept : data_{data} {}

    /**
     * @param count Field width in bits, at most 32.
     * @param value Receives the raw field, still carrying whatever bias the writer applied.
     * @return True when the payload holds that many further bits.
     */
    [[nodiscard]] bool read(std::size_t count, std::uint32_t& value) noexcept {
        if (count > kMaxFieldBits || position_ + count > data_.size() * 8U) {
            return false;
        }
        std::uint32_t accumulator = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t byteIndex = position_ >> 3U;
            const unsigned shift = 7U - static_cast<unsigned>(position_ & 7U);
            const auto raw = std::to_integer<unsigned>(data_[byteIndex]);
            const auto bit = static_cast<std::uint32_t>((raw >> shift) & 1U);
            accumulator = (accumulator << 1U) | bit;
            ++position_;
        }
        value = accumulator;
        return true;
    }

private:
    std::span<const std::byte> data_;
    std::size_t position_{};
};

/**
 * Reads one 1-bit presence flag followed by an 8-bit biased value.
 * @param reader In-out bit cursor.
 * @param value Receives the unbiased value.
 * @return True when the field is present and readable. An absent field fails closed: nothing on
 *         the wire carries the value that was skipped, so there is nothing to fall back to.
 */
[[nodiscard]] bool read_optional_byte(BitReader& reader, std::uint8_t& value) noexcept {
    std::uint32_t present = 0;
    if (!reader.read(1, present) || present == 0) {
        return false;
    }
    std::uint32_t raw = 0;
    if (!reader.read(8, raw)) {
        return false;
    }
    value = static_cast<std::uint8_t>(raw - kByteBias);
    return true;
}

} // namespace

/** Parses the create-character request body. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() * 8U < kIdentityBits) {
        return false;
    }

    BitReader reader{message.payload};
    if (!read_optional_byte(reader, request.race) || !read_optional_byte(reader, request.gender)
        || !read_optional_byte(reader, request.characterClass)) {
        return false;
    }
    return request.race <= kMaxRace && request.gender <= kMaxGender
           && request.characterClass <= kMaxClass;
}

} // namespace sunrise::middleware::web_service::messages::opcode501
