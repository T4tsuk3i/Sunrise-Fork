/**
 * Persists the complete AccountState to a state.json file beside the loaded module. The file
 * survives restarts so every equipment, inventory, profile-item, and character mutation persists
 * across sessions. The settings.json "state" block seeds the first boot; state.json overrides it
 * from then on.
 *
 * The file uses the same hand-rolled JSON conventions as settings.json and the other stores.
 * Writes are atomic: staged to a .new file and moved over the target.
 */

#include "state_persistence.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../storage/internal.h"

namespace sunrise::state::runtime::persistence {
namespace {

/** The state file lives beside settings.json in the Sunrise artifact directory. */
constexpr std::wstring_view kFileSuffix = L"\\state.json";
/** Staging suffix for atomic writes. */
constexpr std::wstring_view kStageSuffix = L".new";
/**
 * Longest form one item, profile row and character can take in this document: every field
 * present, every plug filled, with slack for the indentation the emitter adds.
 */
constexpr std::size_t kItemBudget = 512;
constexpr std::size_t kProfileItemBudget = 128;
constexpr std::size_t kCharacterBudget =
    1024
    + (account::inventory::kEquipmentSlotCount + account::inventory::kCharacterItemCapacity)
          * kItemBudget;
/**
 * Largest state file accepted, derived from the capacities above rather than picked, so it
 * cannot silently become too small when one of them grows.
 */
constexpr std::size_t kFileCapacity =
    4096 + account::inventory::kProfileItemCapacity * kProfileItemBudget
    + state::kCharacterCapacity * kCharacterBudget;

core::path::Buffer g_path{};
bool g_pathResolved{};

// ---------------------------------------------------------------------------
// JSON emitter — builds a growable document via append operations.
// ---------------------------------------------------------------------------

struct Document {
    std::vector<char> buf{};
};

void open_object(Document& doc) noexcept {
    doc.buf.push_back('{');
}
void close_object(Document& doc) noexcept {
    doc.buf.push_back('}');
}
void open_array(Document& doc) noexcept {
    doc.buf.push_back('[');
}
void close_array(Document& doc) noexcept {
    doc.buf.push_back(']');
}
void comma(Document& doc) noexcept {
    doc.buf.push_back(',');
}
void colon(Document& doc) noexcept {
    doc.buf.push_back(':');
}
void newline(Document& doc) noexcept {
    doc.buf.push_back('\n');
}
void indent(Document& doc, int depth) noexcept {
    for (int i = 0; i < depth; ++i) {
        doc.buf.push_back(' ');
        doc.buf.push_back(' ');
    }
}

void emit_quoted(Document& doc, const char* key) noexcept {
    doc.buf.push_back('"');
    const std::size_t len = std::strlen(key);
    doc.buf.insert(doc.buf.end(), key, key + len);
    doc.buf.push_back('"');
}

void emit_hex(Document& doc, std::uint64_t value) noexcept {
    char buf[32]{};
    const int len =
        std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(value));
    if (len > 0) {
        doc.buf.insert(doc.buf.end(), buf, buf + len);
    }
}

void emit_uint(Document& doc, std::uint64_t value) noexcept {
    char buf[32]{};
    const int len = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    if (len > 0) {
        doc.buf.insert(doc.buf.end(), buf, buf + len);
    }
}

void emit_int(Document& doc, std::int32_t value) noexcept {
    char buf[32]{};
    const int len = std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(value));
    if (len > 0) {
        doc.buf.insert(doc.buf.end(), buf, buf + len);
    }
}

void emit_float(Document& doc, float value) noexcept {
    char buf[64]{};
    const int len = std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
    if (len > 0) {
        doc.buf.insert(doc.buf.end(), buf, buf + len);
    }
}

void emit_bool(Document& doc, bool value) noexcept {
    const char* text = value ? "true" : "false";
    doc.buf.insert(doc.buf.end(), text, text + std::strlen(text));
}

void emit_key(Document& doc, const char* key, int depth) noexcept {
    indent(doc, depth);
    emit_quoted(doc, key);
    colon(doc);
}

// ---------------------------------------------------------------------------
// Emission of nested account types.
// ---------------------------------------------------------------------------

/** Emits the captured appearance header, or null for a character that never carried one. */
void emit_appearance_header(Document& doc, const state::CharacterState& c) noexcept {
    if (!c.appearanceHeaderValid) {
        const char* n = "null";
        doc.buf.insert(doc.buf.end(), n, n + 4);
        return;
    }
    open_array(doc);
    for (std::size_t i = 0; i < c.appearanceHeader.size(); ++i) {
        if (i > 0) comma(doc);
        emit_uint(doc, c.appearanceHeader[i]);
    }
    close_array(doc);
}

void emit_sockets(Document& doc,
                  const account::inventory::Sockets& sockets,
                  int /*depth*/) noexcept {
    if (sockets.policy == account::inventory::SocketPolicy::nativeDefaults) {
        const char* n = "null";
        doc.buf.insert(doc.buf.end(), n, n + 4);
        return;
    }
    open_array(doc);
    for (std::size_t i = 0; i < sockets.plugCount; ++i) {
        if (i > 0) comma(doc);
        if (sockets.plugs[i].has_value()) {
            emit_uint(doc, *sockets.plugs[i]);
        } else {
            const char* n = "null";
            doc.buf.insert(doc.buf.end(), n, n + 4);
        }
    }
    close_array(doc);
}

void emit_item(Document& doc, const account::inventory::Item& item, int depth) noexcept {
    open_object(doc);
    newline(doc);
    emit_key(doc, "instance_soid", depth + 1);
    emit_hex(doc, item.instanceSoid);
    comma(doc);
    newline(doc);
    emit_key(doc, "definition_hash", depth + 1);
    emit_uint(doc, item.definitionHash);
    comma(doc);
    newline(doc);
    emit_key(doc, "level", depth + 1);
    emit_int(doc, item.level);
    comma(doc);
    newline(doc);
    emit_key(doc, "quantity", depth + 1);
    emit_int(doc, item.quantity);
    comma(doc);
    newline(doc);
    emit_key(doc, "mutation_serial", depth + 1);
    emit_int(doc, item.mutationSerial);
    comma(doc);
    newline(doc);
    emit_key(doc, "flags", depth + 1);
    emit_uint(doc, item.flags);
    comma(doc);
    newline(doc);
    emit_key(doc, "plugs", depth + 1);
    emit_sockets(doc, item.sockets, depth + 1);
    newline(doc);
    indent(doc, depth);
    close_object(doc);
}

void emit_equipment(Document& doc, const account::inventory::Equipment& equip, int depth) noexcept {
    open_object(doc);
    bool first = true;
    constexpr const char* kSlotNames[] = {"kinetic",
                                          "energy",
                                          "heavy",
                                          "helmet",
                                          "gauntlets",
                                          "chest",
                                          "legs",
                                          "class_item",
                                          "ghost",
                                          "vehicle",
                                          "ship",
                                          "subclass",
                                          "clan_banner",
                                          "emblem",
                                          "emote",
                                          "finisher"};
    for (std::size_t i = 0; i < account::inventory::kEquipmentSlotCount; ++i) {
        if (!equip.slots[i].has_value()) {
            continue;
        }
        if (!first) {
            comma(doc);
            newline(doc);
        }
        first = false;
        indent(doc, depth + 1);
        emit_quoted(doc, kSlotNames[i]);
        colon(doc);
        emit_item(doc, *equip.slots[i], depth + 1);
    }
    newline(doc);
    indent(doc, depth);
    close_object(doc);
}

void emit_character_inventory(Document& doc,
                              const account::inventory::CharacterItems& items,
                              int depth) noexcept {
    open_array(doc);
    for (std::size_t i = 0; i < items.count; ++i) {
        if (i > 0) {
            comma(doc);
            newline(doc);
            indent(doc, depth + 1);
        } else {
            newline(doc);
            indent(doc, depth + 1);
        }
        emit_item(doc, items.values[i], depth + 1);
    }
    if (items.count > 0) {
        newline(doc);
        indent(doc, depth);
    }
    close_array(doc);
}

void emit_character(Document& doc, const state::CharacterState& c, int depth) noexcept {
    open_object(doc);
    newline(doc);

    emit_key(doc, "soid", depth + 1);
    emit_hex(doc, c.soid);
    comma(doc);
    newline(doc);
    emit_key(doc, "race", depth + 1);
    emit_uint(doc, static_cast<std::uint8_t>(c.race));
    comma(doc);
    newline(doc);
    emit_key(doc, "gender", depth + 1);
    emit_uint(doc, static_cast<std::uint8_t>(c.gender));
    comma(doc);
    newline(doc);
    emit_key(doc, "class", depth + 1);
    emit_uint(doc, static_cast<std::uint8_t>(c.characterClass));
    comma(doc);
    newline(doc);
    emit_key(doc, "level", depth + 1);
    emit_uint(doc, c.level);
    comma(doc);
    newline(doc);
    emit_key(doc, "accepted", depth + 1);
    emit_bool(doc, c.accepted);
    comma(doc);
    newline(doc);
    emit_key(doc, "preview_available", depth + 1);
    emit_bool(doc, c.previewAvailable);
    comma(doc);
    newline(doc);
    emit_key(doc, "appearance_value", depth + 1);
    emit_float(doc, c.appearanceValue);
    comma(doc);
    newline(doc);
    emit_key(doc, "last_orbited_destination", depth + 1);
    emit_uint(doc, c.lastOrbitedDestination);
    comma(doc);
    newline(doc);
    emit_key(doc, "content_bypass", depth + 1);
    emit_bool(doc, c.contentBypass);
    comma(doc);
    newline(doc);
    emit_key(doc, "appearance_header", depth + 1);
    emit_appearance_header(doc, c);
    comma(doc);
    newline(doc);
    // The ability entries are no longer character fields: they live on the subclass item and so
    // travel inside the equipment written below.
    emit_key(doc, "equipment", depth + 1);
    emit_equipment(doc, c.equipment, depth + 1);
    comma(doc);
    newline(doc);
    emit_key(doc, "inventory", depth + 1);
    emit_character_inventory(doc, c.inventory, depth + 1);
    newline(doc);

    indent(doc, depth);
    close_object(doc);
}

void emit_profile_item(Document& doc,
                       const account::inventory::ProfileItem& item,
                       int depth) noexcept {
    open_object(doc);
    emit_key(doc, "definition_hash", depth + 1);
    emit_uint(doc, item.definitionHash);
    comma(doc);
    emit_key(doc, "quantity", depth + 1);
    emit_int(doc, item.quantity);
    close_object(doc);
}

void emit_profile_items(Document& doc, const state::AccountState& account, int depth) noexcept {
    open_array(doc);
    for (std::size_t i = 0; i < account.profileItemCount; ++i) {
        if (i > 0) {
            comma(doc);
            newline(doc);
            indent(doc, depth + 1);
        } else {
            newline(doc);
            indent(doc, depth + 1);
        }
        emit_profile_item(doc, account.profileItems[i], depth + 1);
    }
    if (account.profileItemCount > 0) {
        newline(doc);
        indent(doc, depth);
    }
    close_array(doc);
}

void emit_dismantle_reward(Document& doc,
                           const state::DismantleRewardPolicy& r,
                           int depth) noexcept {
    open_object(doc);
    emit_key(doc, "definition_hash", depth + 1);
    emit_uint(doc, r.definitionHash);
    comma(doc);
    emit_key(doc, "quantity", depth + 1);
    emit_int(doc, r.quantity);
    close_object(doc);
}

void emit_dismantle_rewards(Document& doc, const state::AccountState& account, int depth) noexcept {
    open_array(doc);
    for (std::size_t i = 0; i < account.dismantleRewardCount; ++i) {
        if (i > 0) {
            comma(doc);
            newline(doc);
            indent(doc, depth + 1);
        } else {
            newline(doc);
            indent(doc, depth + 1);
        }
        emit_dismantle_reward(doc, account.dismantleRewards[i], depth + 1);
    }
    if (account.dismantleRewardCount > 0) {
        newline(doc);
        indent(doc, depth);
    }
    close_array(doc);
}

void emit_characters(Document& doc, const state::AccountState& account, int depth) noexcept {
    open_array(doc);
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        if (i > 0) {
            comma(doc);
            newline(doc);
            indent(doc, depth + 1);
        } else {
            newline(doc);
            indent(doc, depth + 1);
        }
        emit_character(doc, account.characters[i], depth + 1);
    }
    if (account.characterCount > 0) {
        newline(doc);
        indent(doc, depth);
    }
    close_array(doc);
}

void emit_account(Document& doc, const state::AccountState& account) noexcept {
    open_object(doc);
    newline(doc);

    indent(doc, 1);
    emit_quoted(doc, "version");
    colon(doc);
    emit_uint(doc, kStateVersion);
    comma(doc);
    newline(doc);
    indent(doc, 1);
    emit_quoted(doc, "primary_soid");
    colon(doc);
    emit_hex(doc, account.primarySoid);
    comma(doc);
    newline(doc);
    indent(doc, 1);
    emit_quoted(doc, "profile_items");
    colon(doc);
    emit_profile_items(doc, account, 1);
    comma(doc);
    newline(doc);
    indent(doc, 1);
    emit_quoted(doc, "dismantle_rewards");
    colon(doc);
    emit_dismantle_rewards(doc, account, 1);
    comma(doc);
    newline(doc);
    indent(doc, 1);
    emit_quoted(doc, "characters");
    colon(doc);
    emit_characters(doc, account, 1);
    newline(doc);

    close_object(doc);
    newline(doc);
}

// ---------------------------------------------------------------------------
// JSON parser — mirrors the settings parser for the same keys.
// ---------------------------------------------------------------------------

class StateParser {
public:
    explicit StateParser(std::string_view text) noexcept : m_text(text) {}

    /**
     * Layout version the parsed document declared. Stays 0 for a file written before the
     * version was stamped, which load() accepts rather than discarding a working save.
     */
    std::uint64_t documentVersion{};

    [[nodiscard]] bool parse_account(state::AccountState& output) noexcept {
        output = {};
        if (!consume('{')) return false;
        if (consume('}')) return true;
        for (;;) {
            std::string_view key;
            if (!parse_string(key) || !consume(':')) return false;
            if (key == "version") {
                if (!parse_uint(documentVersion)) return false;
            } else if (key == "primary_soid") {
                if (!parse_hex(output.primarySoid)) return false;
            } else if (key == "profile_items") {
                if (!parse_profile_items(output)) return false;
            } else if (key == "dismantle_rewards") {
                if (!parse_dismantle_rewards(output)) return false;
            } else if (key == "characters") {
                if (!parse_characters(output)) return false;
            } else {
                if (!skip_value()) return false;
            }
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

private:
    std::string_view m_text;
    std::size_t m_pos{};

    [[nodiscard]] char peek() const noexcept {
        return m_pos < m_text.size() ? m_text[m_pos] : '\0';
    }

    void advance() noexcept {
        if (m_pos < m_text.size()) ++m_pos;
    }

    [[nodiscard]] bool consume(char c) noexcept {
        skip_whitespace();
        if (m_pos < m_text.size() && m_text[m_pos] == c) {
            ++m_pos;
            return true;
        }
        return false;
    }

    void skip_whitespace() noexcept {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool parse_string(std::string_view& output) noexcept {
        skip_whitespace();
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
        ++m_pos;
        const std::size_t start = m_pos;
        while (m_pos < m_text.size()) {
            if (m_text[m_pos] == '\\') {
                m_pos += 2;
                continue;
            }
            if (m_text[m_pos] == '"') {
                output = m_text.substr(start, m_pos - start);
                ++m_pos;
                return true;
            }
            ++m_pos;
        }
        return false;
    }

    [[nodiscard]] bool parse_hex(std::uint64_t& output) noexcept {
        skip_whitespace();
        if (m_pos + 2 >= m_text.size() || m_text[m_pos] != '0' || m_text[m_pos + 1] != 'x') {
            return false;
        }
        m_pos += 2;
        output = 0;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c >= '0' && c <= '9') {
                output = output * 16 + (c - '0');
                ++m_pos;
            } else if (c >= 'a' && c <= 'f') {
                output = output * 16 + (c - 'a' + 10);
                ++m_pos;
            } else if (c >= 'A' && c <= 'F') {
                output = output * 16 + (c - 'A' + 10);
                ++m_pos;
            } else
                break;
        }
        return true;
    }

    [[nodiscard]] bool parse_uint(std::uint64_t& output) noexcept {
        skip_whitespace();
        if (m_pos >= m_text.size()) return false;
        output = 0;
        bool started = false;
        while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
            output = output * 10 + (m_text[m_pos] - '0');
            ++m_pos;
            started = true;
        }
        return started;
    }

    [[nodiscard]] bool parse_int(std::int32_t& output) noexcept {
        skip_whitespace();
        bool negative = false;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            negative = true;
            ++m_pos;
        }
        std::uint64_t magnitude = 0;
        if (!parse_uint(magnitude)) return false;
        if (negative) {
            output = -static_cast<std::int32_t>(magnitude);
        } else {
            output = static_cast<std::int32_t>(magnitude);
        }
        return true;
    }

    [[nodiscard]] bool parse_bool(bool& output) noexcept {
        skip_whitespace();
        if (m_text.compare(m_pos, 4, "true") == 0) {
            output = true;
            m_pos += 4;
            return true;
        }
        if (m_text.compare(m_pos, 5, "false") == 0) {
            output = false;
            m_pos += 5;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parse_float(float& output) noexcept {
        skip_whitespace();
        const std::size_t start = m_pos;
        while (m_pos < m_text.size() && m_text[m_pos] != ',' && m_text[m_pos] != '}'
               && m_text[m_pos] != ']' && m_text[m_pos] != ' ' && m_text[m_pos] != '\t'
               && m_text[m_pos] != '\r' && m_text[m_pos] != '\n') {
            ++m_pos;
        }
        if (m_pos == start) return false;
        const std::string_view token = m_text.substr(start, m_pos - start);
        char* end = nullptr;
        const double value = std::strtod(token.data(), &end);
        if (end != token.data() + token.size()) return false;
        output = static_cast<float>(value);
        return true;
    }

    [[nodiscard]] bool parse_null() noexcept {
        skip_whitespace();
        if (m_text.compare(m_pos, 4, "null") == 0) {
            m_pos += 4;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool skip_value() noexcept {
        skip_whitespace();
        if (m_pos >= m_text.size()) return false;
        const char c = m_text[m_pos];
        if (c == '"') {
            std::string_view dummy;
            return parse_string(dummy);
        }
        if (c == '{') return skip_object();
        if (c == '[') return skip_array();
        if (c == 'n') return parse_null();
        // number or bool
        while (m_pos < m_text.size() && m_text[m_pos] != ',' && m_text[m_pos] != '}'
               && m_text[m_pos] != ']') {
            ++m_pos;
        }
        return true;
    }

    [[nodiscard]] bool skip_object() noexcept {
        if (!consume('{')) return false;
        if (consume('}')) return true;
        for (;;) {
            std::string_view key;
            if (!parse_string(key) || !consume(':') || !skip_value()) return false;
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool skip_array() noexcept {
        if (!consume('[')) return false;
        if (consume(']')) return true;
        for (;;) {
            if (!skip_value()) return false;
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_profile_item(account::inventory::ProfileItem& item) noexcept {
        item = {};
        if (!consume('{')) return false;
        if (consume('}')) return false;
        bool hasHash = false;
        bool hasQuantity = false;
        for (;;) {
            std::string_view key;
            if (!parse_string(key) || !consume(':')) return false;
            if (key == "definition_hash") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                item.definitionHash = static_cast<std::uint32_t>(v);
                hasHash = true;
            } else if (key == "quantity") {
                if (!parse_int(item.quantity)) return false;
                hasQuantity = true;
            } else {
                if (!skip_value()) return false;
            }
            if (consume('}')) return hasHash && hasQuantity;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_profile_items(state::AccountState& output) noexcept {
        output.profileItems = {};
        output.profileItemCount = 0;
        if (!consume('[')) return false;
        if (consume(']')) return true;
        for (;;) {
            if (output.profileItemCount >= output.profileItems.size()) return false;
            if (!parse_profile_item(output.profileItems[output.profileItemCount])) return false;
            ++output.profileItemCount;
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_dismantle_reward(state::DismantleRewardPolicy& reward) noexcept {
        reward = {};
        if (!consume('{')) return false;
        if (consume('}')) return false;
        bool hasHash = false;
        bool hasQuantity = false;
        for (;;) {
            std::string_view key;
            if (!parse_string(key) || !consume(':')) return false;
            if (key == "definition_hash") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                reward.definitionHash = static_cast<std::uint32_t>(v);
                hasHash = true;
            } else if (key == "quantity") {
                if (!parse_int(reward.quantity)) return false;
                hasQuantity = true;
            } else {
                if (!skip_value()) return false;
            }
            if (consume('}')) return hasHash && hasQuantity;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_dismantle_rewards(state::AccountState& output) noexcept {
        output.dismantleRewards = {};
        output.dismantleRewardCount = 0;
        if (!consume('[')) return false;
        if (consume(']')) return true;
        for (;;) {
            if (output.dismantleRewardCount >= output.dismantleRewards.size()) return false;
            if (!parse_dismantle_reward(output.dismantleRewards[output.dismantleRewardCount]))
                return false;
            ++output.dismantleRewardCount;
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_sockets(account::inventory::Sockets& output) noexcept {
        output = {};
        if (parse_null()) return true;
        if (!consume('[')) return false;
        output.policy = account::inventory::SocketPolicy::authored;
        if (consume(']')) return true;
        for (;;) {
            if (output.plugCount >= output.plugs.size()) return false;
            if (parse_null()) {
                output.plugs[output.plugCount].reset();
            } else {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                output.plugs[output.plugCount] = static_cast<std::uint32_t>(v);
            }
            ++output.plugCount;
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_item(account::inventory::Item& item) noexcept {
        item = {};
        if (!consume('{')) return false;
        if (consume('}')) return false;
        for (;;) {
            std::string_view key;
            if (!parse_string(key) || !consume(':')) return false;
            if (key == "instance_soid") {
                if (!parse_hex(item.instanceSoid)) return false;
            } else if (key == "definition_hash") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                item.definitionHash = static_cast<std::uint32_t>(v);
            } else if (key == "level") {
                if (!parse_int(item.level)) return false;
            } else if (key == "quantity") {
                if (!parse_int(item.quantity)) return false;
            } else if (key == "mutation_serial") {
                if (!parse_int(item.mutationSerial)) return false;
            } else if (key == "flags") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                item.flags = static_cast<std::uint32_t>(v);
            } else if (key == "plugs") {
                if (!parse_sockets(item.sockets)) return false;
            } else {
                if (!skip_value()) return false;
            }
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_equipment(account::inventory::Equipment& output) noexcept {
        output = {};
        if (!consume('{')) return false;
        if (consume('}')) return true;
        for (;;) {
            std::string_view key;
            if (!parse_string(key) || !consume(':')) return false;
            const std::optional<account::inventory::EquipmentSlot> slot =
                account::inventory::slot_from_name(key);
            if (!slot.has_value()) return false;
            const std::size_t index = static_cast<std::size_t>(*slot);
            if (parse_null()) {
                output.slots[index].reset();
            } else {
                account::inventory::Item item{};
                if (!parse_item(item)) return false;
                output.slots[index] = item;
            }
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool
    parse_character_inventory(account::inventory::CharacterItems& output) noexcept {
        output = {};
        if (!consume('[')) return false;
        if (consume(']')) return true;
        for (;;) {
            if (output.count >= output.values.size()) return false;
            if (!parse_item(output.values[output.count])) return false;
            ++output.count;
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    /** Parses the captured appearance header; null leaves the character on the shared block. */
    [[nodiscard]] bool parse_appearance_header(state::CharacterState& output) noexcept {
        output.appearanceHeader = {};
        output.appearanceHeaderValid = false;
        if (parse_null()) return true;
        if (!consume('[')) return false;
        std::size_t count = 0;
        for (;;) {
            if (count >= output.appearanceHeader.size()) return false;
            std::uint64_t v = 0;
            if (!parse_uint(v)) return false;
            output.appearanceHeader[count] = static_cast<std::uint8_t>(v);
            ++count;
            if (consume(']')) break;
            if (!consume(',')) return false;
        }
        // A short block cannot be padded into a valid header, so it is refused outright rather
        // than published half-formed.
        if (count != output.appearanceHeader.size()) return false;
        output.appearanceHeaderValid = true;
        return true;
    }

    [[nodiscard]] bool parse_character(state::CharacterState& output) noexcept {
        output = {};
        if (!consume('{')) return false;
        if (consume('}')) return false;
        for (;;) {
            std::string_view key;
            if (!parse_string(key) || !consume(':')) return false;
            if (key == "soid") {
                if (!parse_hex(output.soid)) return false;
            } else if (key == "race") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                output.race = static_cast<state::CharacterRace>(v);
            } else if (key == "gender") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                output.gender = static_cast<state::CharacterGender>(v);
            } else if (key == "class") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                output.characterClass = static_cast<state::CharacterClass>(v);
            } else if (key == "level") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                output.level = static_cast<std::uint8_t>(v);
            } else if (key == "accepted") {
                if (!parse_bool(output.accepted)) return false;
            } else if (key == "preview_available") {
                if (!parse_bool(output.previewAvailable)) return false;
            } else if (key == "appearance_value") {
                if (!parse_float(output.appearanceValue)) return false;
            } else if (key == "last_orbited_destination") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return false;
                output.lastOrbitedDestination = static_cast<std::uint32_t>(v);
            } else if (key == "content_bypass") {
                if (!parse_bool(output.contentBypass)) return false;
            } else if (key == "appearance_header") {
                if (!parse_appearance_header(output)) return false;
                // The ability-entry keys a file written before those fields moved onto the subclass
                // item still carries fall through to skip_value() below, so an older save still
                // loads.
            } else if (key == "equipment") {
                if (!parse_equipment(output.equipment)) return false;
            } else if (key == "inventory") {
                if (!parse_character_inventory(output.inventory)) return false;
            } else {
                if (!skip_value()) return false;
            }
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

    [[nodiscard]] bool parse_characters(state::AccountState& output) noexcept {
        output.characters = {};
        output.characterCount = 0;
        if (!consume('[')) return false;
        if (consume(']')) return true;
        for (;;) {
            if (output.characterCount >= output.characters.size()) return false;
            if (!parse_character(output.characters[output.characterCount])) return false;
            ++output.characterCount;
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }
};

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=state_persistence stage=persist result=fail reason=%s",
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool write_document(const std::vector<char>& doc) noexcept {
    if (!g_pathResolved || doc.empty()) return false;

    // Stage to .new file, then atomically move over the target.
    core::path::Buffer stagePath = g_path;
    if (!core::path::append(stagePath, kStageSuffix)) {
        report_fail("stage_path");
        return false;
    }

    const HANDLE file = CreateFileW(stagePath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report_fail("open");
        return false;
    }

    DWORD written = 0;
    const auto size = static_cast<DWORD>(doc.size());
    bool complete =
        WriteFile(file, doc.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;

    if (complete) {
        complete =
            MoveFileExW(stagePath.chars.data(), g_path.chars.data(), MOVEFILE_REPLACE_EXISTING)
            != FALSE;
    }
    if (!complete) {
        (void)DeleteFileW(stagePath.chars.data());
        report_fail("write");
    }
    return complete;
}

[[nodiscard]] bool read_document(std::vector<char>& doc) noexcept {
    if (!g_pathResolved) return false;

    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0
        || static_cast<std::uint64_t>(fileSize.QuadPart) > kFileCapacity) {
        CloseHandle(file);
        return false;
    }

    doc.resize(static_cast<std::size_t>(fileSize.QuadPart));
    DWORD read = 0;
    const bool ok =
        ReadFile(file, doc.data(), static_cast<DWORD>(fileSize.QuadPart), &read, nullptr) != FALSE
        && read == static_cast<DWORD>(fileSize.QuadPart);
    (void)CloseHandle(file);
    if (!ok) {
        doc.clear();
        return false;
    }
    return true;
}

} // namespace

bool initialize(void* module) noexcept {
    g_path = core::path::Buffer{};
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    if (!g_pathResolved) {
        report_fail("path");
    }
    return true;
}

bool load(state::AccountState& output) noexcept {
    std::vector<char> doc;
    if (!read_document(doc)) {
        // No file or read failure — caller keeps the settings defaults.
        return true;
    }

    StateParser parser(std::string_view(doc.data(), doc.size()));
    state::AccountState parsed{};
    if (!parser.parse_account(parsed)) {
        report_fail("parse");
        return true; // Non-fatal: keep settings defaults.
    }
    // A version this build does not know is refused rather than migrated, which is the safe
    // direction: the authored account is already good. 0 is a file written before versioning.
    if (parser.documentVersion != 0 && parser.documentVersion != kStateVersion) {
        report_fail("version");
        return true;
    }
    // A save belongs to the account it was written on. Adopting another one's characters and
    // balances would invent them out of a stale file.
    if (output.primarySoid != 0 && parsed.primarySoid != output.primarySoid) {
        report_fail("identity");
        return true;
    }
    // state.json deliberately carries no AccountSettings: those are configuration, owned by
    // settings.json. account::valid() requires them, so the authored ones are carried onto the
    // candidate here -- validating the parsed account on its own would reject every save.
    parsed.settings = output.settings;
    // The authored account is already known good, so a document that cannot produce a valid one
    // is dropped whole rather than installed and left to fail somewhere further downstream.
    if (!account::valid(parsed)) {
        report_fail("invalid");
        return true;
    }
    output = parsed;
    return true;
}

bool save() noexcept {
    // Snapshot the account under the lock, then write outside it.
    state::AccountState snapshot{};
    {
        AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
        snapshot = runtime::storage::g_state.account;
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    }

    Document doc{};
    doc.buf.reserve(kFileCapacity);
    emit_account(doc, snapshot);

    return write_document(doc.buf);
}

void shutdown() noexcept {
    g_path = core::path::Buffer{};
    g_pathResolved = false;
}

} // namespace sunrise::state::runtime::persistence
