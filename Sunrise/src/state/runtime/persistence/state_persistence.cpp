/**
 * Persists the complete AccountState to a state.json file beside the loaded module. The file
 * survives restarts so every equipment, inventory, profile-item, and character mutation persists
 * across sessions. The settings.json "state" block seeds the first boot; state.json overrides it
 * from then on.
 *
 * The file uses the same hand-rolled JSON conventions as settings.json and the other stores.
 * Writes are atomic: staged to a .new file and moved over the target. Before that, the two most
 * recent generations are kept as best-effort .bak / .bak2 backups, each with its own .sum
 * checksum sidecar (FNV-1a) that travels with it through the rotation. A source whose sidecar
 * does not match is treated the same as one that fails to parse: `load` moves on to the next.
 *
 * A mutation does not write to disk itself: it marks the account dirty (`request_save`) and a
 * background thread flushes it within `kSaveIntervalMs`. Writing the whole account on every
 * single equip, dismantle or socket plug would put a full serialize-and-write on the critical
 * path of every mutation; a short-lived flush thread coalesces a burst of mutations into one
 * write instead.
 */

#include "state_persistence.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../storage/internal.h"

namespace sunrise::state::runtime::persistence {
namespace {

/** The state file lives beside settings.json in the Sunrise artifact directory. */
constexpr std::wstring_view kFileSuffix = L"\\state.json";
/** Staging suffix for atomic writes. */
constexpr std::wstring_view kStageSuffix = L".new";
/** Suffix of the newest best-effort backup: the generation the most recent save replaced. */
constexpr std::wstring_view kBackupSuffix = L".bak";
/** Suffix of the older best-effort backup: the generation before that one. */
constexpr std::wstring_view kBackup2Suffix = L".bak2";
/**
 * Suffix of a source file's checksum sidecar, appended to that source's own path.
 * Kept as a fixed-width hex sidecar rather than a field inside the JSON: folding it into the
 * document would need excluding the field's own bytes from what it covers, which the emitter's
 * single forward pass cannot do without a second pass over the buffer. A missing or unreadable
 * sidecar is never a failure on its own -- an older save predates this feature -- only a mismatch
 * against a sidecar that IS there is.
 */
constexpr std::wstring_view kChecksumSuffix = L".sum";
/** FNV-1a constants, the same ones this codebase already uses elsewhere for content fingerprints. */
constexpr std::uint64_t kHashOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;
/** Hex digits an emitted checksum sidecar holds: one 64-bit FNV-1a value. */
constexpr std::size_t kChecksumHexDigits = 16;
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
/**
 * A document at or past this size gets a warning on an otherwise-successful save.
 * `flush_now` already refuses to write past `kFileCapacity` outright; this exists so that
 * refusal is never the first sign of trouble; the log carries a standing warning for as long as
 * the account stays this full, which is the cue to raise the budgets above before it is reached.
 */
constexpr std::size_t kCapacityWarnThreshold = kFileCapacity * 9 / 10;

core::path::Buffer g_path{};
bool g_pathResolved{};

/**
 * Flush thread state. Two flushes must never run concurrently, and the design already guarantees
 * that: only `flush_thread` calls `flush_now`, `shutdown` never calls it directly, and a mutation
 * arriving with no thread running (creation failed at boot) writes synchronously from
 * `request_save` on the caller's own thread instead of ever touching the flag below.
 */
std::atomic<bool> g_dirty{};
std::atomic<bool> g_running{};
bool g_threadReady{};
HANDLE g_thread{};
HANDLE g_wakeEvent{};

/**
 * What one flush attempt produced.
 * The distinction matters to `flush_if_dirty`: a transient failure (a locked file, a momentarily
 * busy disk) is worth retrying on the next tick, but a permanent one (the account no longer fits
 * `kFileCapacity`) is not -- it will not resolve itself, and retrying it every tick forever would
 * just repeat the same full serialize-and-allocate for nothing until the capacity constants or
 * the account itself change. `report_capacity_high` already gives standing warning before that
 * point is ever reached, which is the actionable signal for a permanent failure, not a busy loop.
 */
enum class FlushResult : std::uint8_t { written, transientFailure, permanentFailure };

[[nodiscard]] FlushResult flush_now() noexcept;

/**
 * Flushes once if the account is dirty, leaving the flag set only on a transient failure.
 * Clearing the flag before the write, as a plain exchange would, drops a transient failure's
 * mutation until an unrelated future one happens to mark the account dirty again; clearing it
 * unconditionally after a permanent one, on the other hand, would busy-retry a problem that
 * cannot resolve itself. Only `written` and `permanentFailure` clear the flag.
 */
void flush_if_dirty() noexcept {
    if (!g_dirty.load(std::memory_order_acquire)) {
        return;
    }
    if (flush_now() != FlushResult::transientFailure) {
        g_dirty.store(false, std::memory_order_release);
    }
}

/** Wakes on the shorter of the save interval or a shutdown signal, and flushes when dirty. */
DWORD WINAPI flush_thread(LPVOID) noexcept {
    while (g_running.load(std::memory_order_acquire)) {
        WaitForSingleObject(g_wakeEvent, kSaveIntervalMs);
        flush_if_dirty();
    }
    // The wake that ends the loop above still needs to carry a mutation made just before it.
    flush_if_dirty();
    return 0;
}

// ---------------------------------------------------------------------------
// JSON emitter — builds a fixed-capacity document via bounds-checked appends.
// ---------------------------------------------------------------------------

struct Document {
    std::array<char, kFileCapacity> buf{};
    std::size_t size{};
    /** Set once an append would exceed `buf`. The document is never written in that state. */
    bool overflowed{};
};

void append(Document& doc, const char* data, std::size_t length) noexcept {
    if (doc.overflowed || length > doc.buf.size() - doc.size) {
        doc.overflowed = true;
        return;
    }
    std::memcpy(doc.buf.data() + doc.size, data, length);
    doc.size += length;
}
void append(Document& doc, char value) noexcept {
    append(doc, &value, 1);
}

void open_object(Document& doc) noexcept {
    append(doc, '{');
}
void close_object(Document& doc) noexcept {
    append(doc, '}');
}
void open_array(Document& doc) noexcept {
    append(doc, '[');
}
void close_array(Document& doc) noexcept {
    append(doc, ']');
}
void comma(Document& doc) noexcept {
    append(doc, ',');
}
void colon(Document& doc) noexcept {
    append(doc, ':');
}
void newline(Document& doc) noexcept {
    append(doc, '\n');
}
void indent(Document& doc, int depth) noexcept {
    for (int i = 0; i < depth; ++i) {
        append(doc, ' ');
        append(doc, ' ');
    }
}

void emit_quoted(Document& doc, const char* key) noexcept {
    append(doc, '"');
    append(doc, key, std::strlen(key));
    append(doc, '"');
}

void emit_hex(Document& doc, std::uint64_t value) noexcept {
    char buf[32]{};
    const int len =
        std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(value));
    if (len > 0) {
        append(doc, buf, static_cast<std::size_t>(len));
    }
}

void emit_uint(Document& doc, std::uint64_t value) noexcept {
    char buf[32]{};
    const int len = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    if (len > 0) {
        append(doc, buf, static_cast<std::size_t>(len));
    }
}

void emit_int(Document& doc, std::int32_t value) noexcept {
    char buf[32]{};
    const int len = std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(value));
    if (len > 0) {
        append(doc, buf, static_cast<std::size_t>(len));
    }
}

void emit_float(Document& doc, float value) noexcept {
    char buf[64]{};
    const int len = std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
    if (len > 0) {
        append(doc, buf, static_cast<std::size_t>(len));
    }
}

void emit_bool(Document& doc, bool value) noexcept {
    const char* text = value ? "true" : "false";
    append(doc, text, std::strlen(text));
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
        append(doc, "null", 4);
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
        append(doc, "null", 4);
        return;
    }
    open_array(doc);
    for (std::size_t i = 0; i < sockets.plugCount; ++i) {
        if (i > 0) comma(doc);
        if (sockets.plugs[i].has_value()) {
            emit_uint(doc, *sockets.plugs[i]);
        } else {
            append(doc, "null", 4);
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
    // Armor meta is written only by an item that carries some, so a save of ordinary gear keeps
    // the shape it has always had and an absent key still reads back as the documented default.
    if (item.armorArchetype != 0 || item.armorGearTier != 0 || item.armorMasterworkLevel != 0
        || item.armorSetHash != account::inventory::kNoDefinitionHash) {
        emit_key(doc, "armor_archetype", depth + 1);
        emit_uint(doc, item.armorArchetype);
        comma(doc);
        newline(doc);
        emit_key(doc, "armor_gear_tier", depth + 1);
        emit_uint(doc, item.armorGearTier);
        comma(doc);
        newline(doc);
        emit_key(doc, "armor_masterwork_level", depth + 1);
        emit_uint(doc, item.armorMasterworkLevel);
        comma(doc);
        newline(doc);
        emit_key(doc, "armor_set_hash", depth + 1);
        emit_uint(doc, item.armorSetHash);
        comma(doc);
        newline(doc);
    }
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
            } else if (key == "armor_archetype") {
                std::uint64_t v = 0;
                if (!parse_uint(v) || v > account::inventory::kArmorArchetypeMaximum) return false;
                item.armorArchetype = static_cast<std::uint8_t>(v);
            } else if (key == "armor_gear_tier") {
                std::uint64_t v = 0;
                if (!parse_uint(v) || v > account::inventory::kArmorGearTierMaximum) return false;
                item.armorGearTier = static_cast<std::uint8_t>(v);
            } else if (key == "armor_masterwork_level") {
                std::uint64_t v = 0;
                if (!parse_uint(v) || v > account::inventory::kArmorMasterworkMaximum) return false;
                item.armorMasterworkLevel = static_cast<std::uint8_t>(v);
            } else if (key == "armor_set_hash") {
                std::uint64_t v = 0;
                if (!parse_uint(v) || v > (std::numeric_limits<std::uint32_t>::max)()) return false;
                item.armorSetHash = static_cast<std::uint32_t>(v);
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

/** Warns once a save is large enough that `kFileCapacity` is a real ceiling, not a formality. */
void report_capacity_high(std::size_t used, std::size_t capacity) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=state_persistence stage=persist result=near_capacity "
                                      "used=%zu capacity=%zu",
                                      used,
                                      capacity);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Notes a load that took the version-tolerance path rather than an exact match.
 * Only an added or removed field is safe under this path; a field whose meaning changed is not
 * caught here or anywhere else, so this line is the only record that the path was exercised at
 * all -- worth searching for after any change to what an existing field means.
 */
void report_version_tolerance(std::uint32_t documentVersion) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=state_persistence stage=persist result=tolerated document_version=%u build_version=%u",
        static_cast<unsigned>(documentVersion),
        static_cast<unsigned>(kStateVersion));
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Notes that `load` recovered from a backup rather than the primary state file.
 * Loud on purpose: it means state.json itself was unreadable or invalid, and whatever changed in
 * the session since that backup was taken is gone. The player should be able to tell why their
 * most recent progress looks missing, not have it happen silently.
 */
void report_recovered(const char* source) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=state_persistence stage=persist result=recovered source=%s",
        source);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** FNV-1a over one document's bytes, folded the same way this codebase already folds it elsewhere. */
[[nodiscard]] std::uint64_t fnv1a(std::span<const char> data) noexcept {
    std::uint64_t hash = kHashOffsetBasis;
    for (const char value : data) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= kHashPrime;
    }
    return hash;
}

/** @return The checksum sidecar path for one source file, or false if the path would not fit. */
[[nodiscard]] bool checksum_path(const core::path::Buffer& source,
                                 core::path::Buffer& output) noexcept {
    output = source;
    return core::path::append(output, kChecksumSuffix);
}

/** Best-effort write of one source's checksum sidecar. A failure here never fails the save. */
void write_checksum(const core::path::Buffer& source, std::span<const char> data) noexcept {
    core::path::Buffer sumPath{};
    if (!checksum_path(source, sumPath)) {
        report_fail("checksum_path");
        return;
    }
    // One extra byte for snprintf's own null terminator; only the leading kChecksumHexDigits of
    // this are ever written to the sidecar file below.
    std::array<char, kChecksumHexDigits + 1> scratch{};
    const int written = std::snprintf(
        scratch.data(), scratch.size(), "%016llX", static_cast<unsigned long long>(fnv1a(data)));
    if (written != static_cast<int>(kChecksumHexDigits)) {
        report_fail("checksum_format");
        return;
    }
    const HANDLE file = CreateFileW(
        sumPath.chars.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report_fail("checksum_open");
        return;
    }
    DWORD written2 = 0;
    const bool ok =
        WriteFile(file, scratch.data(), static_cast<DWORD>(kChecksumHexDigits), &written2, nullptr)
            != FALSE
        && written2 == static_cast<DWORD>(kChecksumHexDigits);
    (void)CloseHandle(file);
    if (!ok) {
        report_fail("checksum_write");
    }
}

/**
 * Reads one source's checksum sidecar, when there is one.
 * A missing or malformed sidecar is not reported and not a failure: every save made before this
 * feature existed, and every `.bak`/`.bak2` rotated from one, has none, and that is expected.
 * @return True when a well-formed checksum was found and parsed into `value`.
 */
[[nodiscard]] bool read_checksum(const core::path::Buffer& source, std::uint64_t& value) noexcept {
    core::path::Buffer sumPath{};
    if (!checksum_path(source, sumPath)) {
        return false;
    }
    const HANDLE file = CreateFileW(
        sumPath.chars.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<char, kChecksumHexDigits> hex{};
    DWORD read = 0;
    const bool ok = ReadFile(file, hex.data(), static_cast<DWORD>(hex.size()), &read, nullptr) != FALSE
                    && read == static_cast<DWORD>(hex.size());
    (void)CloseHandle(file);
    if (!ok) {
        return false;
    }
    value = 0;
    for (const char digit : hex) {
        std::uint64_t nibble = 0;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<std::uint64_t>(digit - '0');
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<std::uint64_t>(digit - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    return true;
}

/**
 * Rotates the two generations of backup one step, using whatever the target still holds from
 * before this save. Must run before the target is replaced below, or it would only ever copy
 * the generation about to be written, backing up nothing. Best-effort throughout: every failure
 * here, including a first-ever save with nothing yet to back up, is logged and left behind, not
 * propagated -- the primary write is what must succeed, not this. Each file's checksum sidecar
 * rotates alongside it, so a recovered backup can still be verified the same way the primary is.
 */
void rotate_backups() noexcept {
    core::path::Buffer backupPath = g_path;
    core::path::Buffer backup2Path = g_path;
    core::path::Buffer backupSumPath{};
    core::path::Buffer backup2SumPath{};
    if (!core::path::append(backupPath, kBackupSuffix)
        || !core::path::append(backup2Path, kBackup2Suffix)
        || !checksum_path(backupPath, backupSumPath)
        || !checksum_path(backup2Path, backup2SumPath)) {
        report_fail("backup_path");
        return;
    }
    // The older backup steps back one generation first, so it is not clobbered by the one about
    // to replace it. A missing .bak (fewer than two saves so far) leaves nothing to move.
    (void)MoveFileExW(backupPath.chars.data(), backup2Path.chars.data(), MOVEFILE_REPLACE_EXISTING);
    (void)MoveFileExW(
        backupSumPath.chars.data(), backup2SumPath.chars.data(), MOVEFILE_REPLACE_EXISTING);
    // The generation this save is about to replace becomes the newest backup. A missing target
    // (the very first save) leaves nothing to copy, which is expected, not a failure.
    if (!CopyFileW(g_path.chars.data(), backupPath.chars.data(), FALSE)
        && GetLastError() != ERROR_FILE_NOT_FOUND) {
        report_fail("backup");
    }
    core::path::Buffer primarySumPath{};
    if (checksum_path(g_path, primarySumPath)
        && !CopyFileW(primarySumPath.chars.data(), backupSumPath.chars.data(), FALSE)
        && GetLastError() != ERROR_FILE_NOT_FOUND) {
        report_fail("backup_checksum");
    }
}

[[nodiscard]] bool write_document(std::span<const char> data) noexcept {
    if (!g_pathResolved || data.empty()) return false;
    rotate_backups();

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
    const auto size = static_cast<DWORD>(data.size());
    bool complete =
        WriteFile(file, data.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;

    if (complete) {
        complete =
            MoveFileExW(stagePath.chars.data(), g_path.chars.data(), MOVEFILE_REPLACE_EXISTING)
            != FALSE;
    }
    if (!complete) {
        (void)DeleteFileW(stagePath.chars.data());
        report_fail("write");
        return false;
    }
    // The primary write is already durable at this point; a checksum sidecar failure is
    // best-effort like the backup rotation above, not a reason to report the save itself failed.
    write_checksum(g_path, data);
    return true;
}

/**
 * Reads one source file whole, distinguishing "not there" from an actual failure.
 * A missing file is the ordinary case for `.bak`/`.bak2` early in an account's life, and for
 * every source on a brand first boot, so it is not reported. Anything else that stops this from
 * producing a usable buffer -- an open failure that is not "missing", a size query failure, an
 * empty file, one too large for `buffer`, or a partial read -- is a real problem and is reported,
 * so `load` falling through every source never does so with nothing in the log to explain why.
 */
[[nodiscard]] bool read_document(const core::path::Buffer& path,
                                 std::array<char, kFileCapacity>& buffer,
                                 std::size_t& length) noexcept {
    length = 0;
    if (!g_pathResolved) return false;

    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            report_fail("open");
        }
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize)) {
        CloseHandle(file);
        report_fail("size");
        return false;
    }
    if (fileSize.QuadPart <= 0) {
        CloseHandle(file);
        report_fail("empty");
        return false;
    }
    if (static_cast<std::uint64_t>(fileSize.QuadPart) > buffer.size()) {
        CloseHandle(file);
        report_fail("oversized");
        return false;
    }

    DWORD read = 0;
    const bool ok =
        ReadFile(file, buffer.data(), static_cast<DWORD>(fileSize.QuadPart), &read, nullptr)
            != FALSE
        && read == static_cast<DWORD>(fileSize.QuadPart);
    (void)CloseHandle(file);
    if (!ok) {
        report_fail("read");
        return false;
    }
    length = static_cast<std::size_t>(fileSize.QuadPart);
    // A checksum sidecar is optional -- absent for every save made before this feature existed --
    // so only a sidecar that IS there and does not match is treated as a failure.
    std::uint64_t expected = 0;
    if (read_checksum(path, expected) && fnv1a({buffer.data(), length}) != expected) {
        report_fail("checksum");
        length = 0;
        return false;
    }
    return true;
}

/**
 * The actual synchronous write. Only the flush thread calls this, plus `request_save` itself on
 * the fallback path where no flush thread is running.
 */
[[nodiscard]] FlushResult flush_now() noexcept {
    // Snapshot the account under the lock, then write outside it.
    state::AccountState snapshot{};
    {
        AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
        snapshot = runtime::storage::g_state.account;
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    }

    // Heap-allocated: at ~kFileCapacity bytes, this is too large to snapshot safely on a thread
    // stack, the same reason AccountState itself is heap-allocated at boot.
    auto doc = std::make_unique<Document>();
    if (!doc) {
        report_fail("alloc");
        // Usually transient: the next tick's allocation attempt costs little and often succeeds
        // once whatever briefly pressured memory has passed.
        return FlushResult::transientFailure;
    }
    emit_account(*doc, snapshot);
    if (doc->overflowed) {
        report_fail("capacity");
        // Not transient: the account does not fit `kFileCapacity` and retrying will not change
        // that. `report_capacity_high` below is meant to give warning long before this is ever
        // reached; reaching it anyway means the budgets need raising, not another attempt.
        return FlushResult::permanentFailure;
    }
    if (doc->size >= kCapacityWarnThreshold) {
        report_capacity_high(doc->size, kFileCapacity);
    }
    return write_document({doc->buf.data(), doc->size}) ? FlushResult::written
                                                         : FlushResult::transientFailure;
}

/**
 * Parses and validates one candidate document against the account it is loading over.
 * Shared by every source `load` tries -- state.json, then `.bak`, then `.bak2` -- so the exact
 * same gauntlet runs regardless of which file actually produced the account.
 * @param text Whole document.
 * @param authored The account as seeded before any file is read; only its identity and settings
 * are read here, so a failed earlier attempt cannot leak a partial result into a later one.
 * @param reason Set to the failed check's name when this returns false, for the caller's log.
 * @param result Receives the validated account. Untouched on failure.
 * @return True when the document produced a valid account.
 */
[[nodiscard]] bool parse_candidate(std::string_view text,
                                   const state::AccountState& authored,
                                   const char*& reason,
                                   state::AccountState& result) noexcept {
    StateParser parser(text);
    state::AccountState parsed{};
    if (!parser.parse_account(parsed)) {
        reason = "parse";
        return false;
    }
    // A version newer than this build knows cannot be interpreted safely: that version may have
    // changed what an existing field means, not only which fields exist. An older or unversioned
    // file (0 predates versioning) is accepted and runs through the same tolerant field-by-field
    // parser new fields already use, which is enough for a purely additive or subtractive change.
    if (parser.documentVersion > kStateVersion) {
        reason = "version";
        return false;
    }
    if (parser.documentVersion != kStateVersion) {
        report_version_tolerance(static_cast<std::uint32_t>(parser.documentVersion));
    }
    // A save belongs to the account it was written on. Adopting another one's characters and
    // balances would invent them out of a stale file.
    if (authored.primarySoid != 0 && parsed.primarySoid != authored.primarySoid) {
        reason = "identity";
        return false;
    }
    // state.json deliberately carries no AccountSettings: those are configuration, owned by
    // settings.json. account::valid() requires them, so the authored ones are carried onto the
    // candidate here -- validating the parsed account on its own would reject every save.
    parsed.settings = authored.settings;
    // The authored account is already known good, so a document that cannot produce a valid one
    // is dropped whole rather than installed and left to fail somewhere further downstream.
    if (!account::valid(parsed)) {
        reason = "invalid";
        return false;
    }
    result = parsed;
    return true;
}

/**
 * Reads and validates one source file, reporting why it was rejected when it exists but fails.
 * A missing file (the common case for `.bak`/`.bak2` early in an account's life, or for every
 * source on a brand first boot) is not reported: there is nothing wrong with a backup that was
 * never yet needed.
 */
[[nodiscard]] bool load_from(const core::path::Buffer& path,
                             std::array<char, kFileCapacity>& buffer,
                             const state::AccountState& authored,
                             state::AccountState& output) noexcept {
    std::size_t length = 0;
    if (!read_document(path, buffer, length)) {
        return false;
    }
    const char* reason = "unknown";
    if (!parse_candidate(std::string_view(buffer.data(), length), authored, reason, output)) {
        report_fail(reason);
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
        return true;
    }
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_wakeEvent == nullptr) {
        report_fail("event");
        return true; // Persistence still works; request_save falls back to a synchronous write.
    }
    g_running.store(true, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, &flush_thread, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        report_fail("thread");
        g_running.store(false, std::memory_order_release);
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
        return true;
    }
    g_threadReady = true;
    return true;
}

bool load(state::AccountState& output) noexcept {
    if (!g_pathResolved) {
        return true; // Nothing resolved to read from — caller keeps the settings defaults.
    }
    auto buffer = std::make_unique<std::array<char, kFileCapacity>>();
    // Heap-allocated for the same reason `flush_now`'s Document is: a full AccountState is too
    // large to duplicate onto this thread's stack, which the read buffer above already is too.
    auto authored = std::make_unique<state::AccountState>(output);
    if (!buffer || !authored) {
        report_fail("alloc");
        return true;
    }

    if (load_from(g_path, *buffer, *authored, output)) {
        return true;
    }
    // state.json was missing, unreadable, or failed validation. Falling straight through to the
    // seeded defaults here is exactly the failure mode real save-corruption write-ups warn
    // about: the first mutation after boot would then overwrite `.bak` with that empty account,
    // and the real save would be gone within a couple of ordinary saves. Trying the backups
    // first is what a valid `.bak`/`.bak2` exist for.
    core::path::Buffer backupPath = g_path;
    if (core::path::append(backupPath, kBackupSuffix)
        && load_from(backupPath, *buffer, *authored, output)) {
        report_recovered("bak");
        return true;
    }
    core::path::Buffer backup2Path = g_path;
    if (core::path::append(backup2Path, kBackup2Suffix)
        && load_from(backup2Path, *buffer, *authored, output)) {
        report_recovered("bak2");
        return true;
    }
    // No candidate produced a valid account. Each attempt already reported why it failed, or
    // (for a source that was never written yet) reported nothing, which is correct.
    return true;
}

[[nodiscard]] bool request_save() noexcept {
    if (!g_pathResolved) return false;
    if (!g_threadReady) {
        // No background thread to catch this later, so the write happens now instead of never.
        return flush_now() == FlushResult::written;
    }
    // Only marks the flag. Signalling the event here would wake the thread on every single
    // mutation, which is a synchronous write moved to another thread, not a coalesced one; the
    // periodic timeout in flush_thread is what actually picks this up. Only `shutdown` signals
    // the event, to skip the rest of that wait for its own final flush.
    g_dirty.store(true, std::memory_order_release);
    return true;
}

void shutdown() noexcept {
    if (g_threadReady) {
        // Signals the loop in flush_thread to make its exit pass, which flushes first if a
        // mutation landed between the last periodic flush and this signal.
        g_running.store(false, std::memory_order_release);
        SetEvent(g_wakeEvent);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        CloseHandle(g_wakeEvent);
        g_thread = nullptr;
        g_wakeEvent = nullptr;
        g_threadReady = false;
    }
    // With no thread, every request_save already wrote synchronously; nothing is owed here.
    g_path = core::path::Buffer{};
    g_pathResolved = false;
}

} // namespace sunrise::state::runtime::persistence
