#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::items::details;

/** @param bucketId Inventory bucket. @return Its equipment slot, or none when not equippable. */
[[nodiscard]] std::optional<std::int8_t> equipment_slot(std::uint8_t bucketId) noexcept {
    state::build_data::inventory::buckets::Descriptor descriptor{};
    if (state::build_data::find_inventory_bucket_descriptor(bucketId, descriptor)
        && descriptor.equipmentSlot
               != state::build_data::inventory::buckets::kUnavailableEquipmentSlot) {
        return descriptor.equipmentSlot;
    }
    return std::nullopt;
}

/** @param row Package row. @return Its cached detail form. */
[[nodiscard]] domain::Definition to_detail(const tables::items::Row& row) noexcept {
    domain::Definition detail{};
    detail.definitionIndex = row.definitionIndex;
    detail.definitionHash = row.definitionHash;
    detail.bucketId = row.bucketId;
    detail.maxStackSize = row.maxStackSize;
    detail.instancedDefinitionState = row.instanced ? domain::InstancedDefinitionState::instanced
                                                    : domain::InstancedDefinitionState::stackable;
    detail.equipmentSlot =
        row.equipmentSlot.has_value() ? row.equipmentSlot : equipment_slot(row.bucketId);
    detail.ordinarySocketState =
        row.hasSockets ? domain::OrdinarySocketState::present : domain::OrdinarySocketState::absent;
    detail.ordinarySocketCount = row.socketCount;
    for (std::size_t lane = 0; lane < detail.initialPlugIndices.size(); ++lane) {
        detail.initialPlugIndices[lane] = row.initialPlugs[lane];
        detail.socketTypes[lane] = row.socketTypes[lane];
    }
    detail.socketEntryListIndex = row.socketEntryListIndex;
    // Every field below comes from the package blob only; the loaded definition is never read.
    const std::size_t stats =
        row.statCount < detail.stats.size() ? row.statCount : detail.stats.size();
    for (std::size_t entry = 0; entry < stats; ++entry) {
        detail.stats[entry] = {row.statRows[entry], row.statValues[entry]};
    }
    detail.statCount = static_cast<std::uint8_t>(stats);
    detail.gearArtIndex = row.gearArtIndex;
    for (std::size_t index = 0; index < detail.artArrangementIndices.size(); ++index) {
        detail.artArrangementIndices[index] = row.artArrangementIndices[index];
    }
    const std::size_t perks = row.sandboxPerkCount < detail.sandboxPerks.size()
                                  ? row.sandboxPerkCount
                                  : detail.sandboxPerks.size();
    for (std::size_t entry = 0; entry < perks; ++entry) {
        detail.sandboxPerks[entry] = row.sandboxPerks[entry];
    }
    detail.sandboxPerkCount = static_cast<std::uint8_t>(perks);
    const std::size_t overrides = row.renderOverrideCount < detail.renderOverrides.size()
                                      ? row.renderOverrideCount
                                      : detail.renderOverrides.size();
    for (std::size_t entry = 0; entry < overrides; ++entry) {
        detail.renderOverrides[entry] = {row.renderOverrides[entry].stage,
                                         row.renderOverrides[entry].key,
                                         row.renderOverrides[entry].value};
    }
    detail.renderOverrideCount = static_cast<std::uint8_t>(overrides);
    return detail;
}

/** The constants blob's own 8-byte prefix comes before every offset the client quotes. */
constexpr std::size_t kConstantsPrefix = 8;
/** Client offset of the stat row the banner's power number is searched by. */
constexpr std::size_t kLightStatRowOffset = 592;
/**
 * Client offsets of the 6 character stat rows, in the two runs the blob stores them in.
 * The client reads these as 6 separate scalars, not as one array, so each is named here.
 */
constexpr std::size_t kCharacterStatRowOffsets[]{593, 594, 595, 622, 623, 624};
/** First client offset the constants dump covers, which is the blob's own start. */
constexpr std::size_t kConstantsDumpFirst = 0;
/**
 * Client offsets the constants dump covers.
 * The measured blob is 852 bytes and only seven scalars are read out of it, so the dump takes the
 * whole thing: it runs only on a re-extracting boot, and asking for a second one costs minutes.
 */
constexpr std::size_t kConstantsDumpLength = 896;
/** Bytes of the blob encoded on one dump line, so a line stays inside the log's capacity. */
constexpr std::size_t kConstantsDumpStride = 32;

/**
 * Investment root slots the sweep walks.
 * The highest slot addressed by name is the socket entry list table at 97, so the sweep runs well
 * past it. A slot the root does not reach simply fails its read and ends the sweep.
 */
constexpr std::size_t kSlotSweepLimit = 160;

/** Hits reported for one searched value in one slot, so a common word cannot flood the log. */
constexpr std::size_t kHashHitsPerSlot = 4;

/**
 * Reports where a searched value appears in one slot's bytes.
 * The search is over raw bytes rather than a parsed row, because the point is to find a table
 * whose layout is not yet known: the offset a hash lands at is what reveals the row stride once
 * several hits from the same table are lined up.
 * @param slot Slot the blob came from.
 * @param blob Table bytes to search.
 * @param settings Client tuning carrying the searched values.
 */
void search_slot_hashes(std::size_t slot,
                        std::span<const std::byte> blob,
                        const core::settings::client::Settings& settings) noexcept {
    if (settings.hashSearchCount == 0 || blob.size() < sizeof(std::uint32_t)) {
        return;
    }
    for (std::size_t entry = 0; entry < settings.hashSearchCount; ++entry) {
        const std::uint32_t wanted = settings.hashSearchValues[entry];
        std::size_t hits = 0;
        for (std::size_t at = 0; at + sizeof(std::uint32_t) <= blob.size() && hits < kHashHitsPerSlot;
             ++at) {
            std::uint32_t value = 0;
            std::memcpy(&value, blob.data() + at, sizeof value);
            if (value != wanted) {
                continue;
            }
            ++hits;
            std::array<char, core::log::kLineCapacity> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=slot_sweep stage=hash slot=%zu value=%u "
                                              "hash=0x%08X at=%zu bytes=%zu",
                                              slot,
                                              wanted,
                                              wanted,
                                              at,
                                              blob.size());
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
}

/**
 * Hex-dumps the leading bytes of one table.
 * The sweep only runs on a boot that re-extracts, so this takes the evidence while that boot is
 * happening rather than requiring another one: a row stride and the position of a hash inside a
 * row are both readable from a table's opening bytes.
 * @param slot Slot the blob came from.
 * @param blob Table bytes.
 * @param wanted Bytes to dump, bounded by the blob's own size.
 */
void dump_slot_bytes(std::size_t slot,
                     std::span<const std::byte> blob,
                     std::uint64_t wanted) noexcept {
    const std::size_t limit =
        blob.size() < wanted ? blob.size() : static_cast<std::size_t>(wanted);
    for (std::size_t at = 0; at < limit; at += kConstantsDumpStride) {
        const std::size_t span =
            limit - at < kConstantsDumpStride ? limit - at : kConstantsDumpStride;
        std::array<char, core::log::kLineCapacity> line{};
        const int header = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=slot_sweep stage=bytes slot=%zu at=%zu span=%zu b=",
                                         slot,
                                         at,
                                         span);
        if (header <= 0) {
            return;
        }
        auto used = static_cast<std::size_t>(header);
        (void)core::log::append_hex(line, used, blob.subspan(at, span));
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
    }
}

/**
 * Row stride of the stat definition table, in bytes.
 * Measured, not assumed: the six character stat hashes land in slot 95 at 144, 176, 208, 240, 272
 * and 304, so consecutive rows sit 32 bytes apart and the hash occupies each row's first word.
 */
constexpr std::size_t kStatRowStride = 32;
/** Words of one row the decode prints, which is the whole row at this stride. */
constexpr std::size_t kStatRowWords = kStatRowStride / sizeof(std::uint32_t);
/** Rows the decode prints, so a table larger than the stat table cannot flood the log. */
constexpr std::size_t kStatRowDecodeLimit = 96;

/**
 * Prints one named table row by row, as words rather than bytes.
 * The hex dump carries the same bytes, but a stat table is only useful once its rows are lined
 * up: the question this answers is which of a row's fields vary with the stat and which are
 * constant, and that is unreadable from a byte stream spanning several rows per line. The stride
 * is the one measured on the stat table, so pointing this at any other table prints nonsense.
 * @param slot Slot the blob came from.
 * @param blob Table bytes.
 * @param rows Leading array descriptor, naming where the rows start and how many there are.
 */
void decode_slot_rows(std::size_t slot,
                      std::span<const std::byte> blob,
                      const tables::Array& rows) noexcept {
    const std::size_t count = rows.count < kStatRowDecodeLimit
                                  ? static_cast<std::size_t>(rows.count)
                                  : kStatRowDecodeLimit;
    for (std::size_t row = 0; row < count; ++row) {
        const std::size_t at = rows.dataOffset + row * kStatRowStride;
        if (at + kStatRowStride > blob.size()) {
            return;
        }
        std::array<char, core::log::kLineCapacity> line{};
        const int header = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=slot_sweep stage=row slot=%zu idx=%zu at=%zu",
                                         slot,
                                         row,
                                         at);
        if (header <= 0) {
            return;
        }
        auto used = static_cast<std::size_t>(header);
        for (std::size_t word = 0; word < kStatRowWords && used + 32 < line.size(); ++word) {
            std::uint32_t value = 0;
            std::memcpy(&value,
                        blob.data() + at + word * sizeof(std::uint32_t),
                        sizeof value);
            const int extra = std::snprintf(line.data() + used,
                                            line.size() - used,
                                            " w%zu=%u/0x%08X",
                                            word,
                                            value,
                                            value);
            if (extra > 0) {
                used += static_cast<std::size_t>(extra);
            }
        }
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
    }
}

/**
 * Reports every investment root slot and the table it holds.
 * Eight slots are addressed by name and nothing has ever enumerated the rest, so a table this
 * build carries stays invisible until something asks. Each slot's blob is read and its leading
 * array described, which is what names the table: the element class identifies the definition
 * kind and the count says how many rows it holds.
 * @param source Package source the tags are read from.
 * @param scratch Reader scratch storage.
 * @param root Investment root blob holding the slot table.
 * @param blob Reused storage for each slot's tag payload.
 */
void dump_investment_slots(const reader::Source& source,
                           reader::Scratch& scratch,
                           std::span<const std::byte> root,
                           std::vector<std::byte>& blob) noexcept {
    for (std::size_t slot = 0; slot < kSlotSweepLimit; ++slot) {
        std::uint32_t tag = 0;
        if (!tables::slot_tag(root, slot, tag)) {
            // Past the slot table's end, so every later slot is absent too.
            return;
        }
        if (tag == 0) {
            continue;
        }
        std::array<char, core::log::kLineCapacity> line{};
        if (!reader::read_tag(source, scratch, tag, blob)) {
            const int failed =
                std::snprintf(line.data(),
                              line.size(),
                              "ev=slot_sweep stage=slot result=unreadable slot=%zu tag=0x%08X",
                              slot,
                              tag);
            if (failed > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(failed)});
            }
            // The blob still holds the previous slot's bytes, so searching or dumping it here
            // would attribute one slot's contents to another.
            continue;
        }
        tables::Array rows{};
        const bool found = tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kTableArrayDescriptor, rows);
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=slot_sweep stage=slot result=ok slot=%zu tag=0x%08X "
                          "bytes=%zu array=%d class=0x%08X rows=%llu data=%zu",
                          slot,
                          tag,
                          blob.size(),
                          static_cast<int>(found),
                          found ? rows.elementClass : 0U,
                          found ? static_cast<unsigned long long>(rows.count) : 0ULL,
                          found ? rows.dataOffset : std::size_t{});
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        const core::settings::client::Settings& tuning = core::settings::get().client;
        search_slot_hashes(slot, std::span<const std::byte>{blob}, tuning);
        // A named slot is taken whole; every other slot honours the per-slot cap.
        const bool whole = tuning.slotDumpIndex >= 0
                           && static_cast<std::size_t>(tuning.slotDumpIndex) == slot;
        dump_slot_bytes(slot,
                        std::span<const std::byte>{blob},
                        whole ? blob.size() : tuning.slotSweepBytes);
        if (whole && found) {
            decode_slot_rows(slot, std::span<const std::byte>{blob}, rows);
        }
    }
}

/**
 * Dumps the constants bytes spanning both stat-row runs, once per boot.
 * Seven scalars are read out of this blob at offsets recovered from the client and nothing else in
 * it has ever been examined. The six rows arrive in two runs separated by a 26-byte gap, which is
 * the same split the working and non-working stats fall into, so the gap's contents and the values
 * either side of it are the evidence for whether those offsets name what they claim to.
 * @param blob Whole investment constants blob, including its 8-byte prefix.
 */
void dump_investment_constants(std::span<const std::byte> blob) noexcept {
    for (std::size_t at = 0; at < kConstantsDumpLength; at += kConstantsDumpStride) {
        const std::size_t start = kConstantsPrefix + kConstantsDumpFirst + at;
        const std::size_t span = blob.size() > start
                                     ? (blob.size() - start < kConstantsDumpStride
                                            ? blob.size() - start
                                            : kConstantsDumpStride)
                                     : 0;
        if (span == 0) {
            return;
        }
        std::array<char, core::log::kLineCapacity> line{};
        const int header = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=constants stage=dump at=%zu span=%zu b=",
                                         kConstantsDumpFirst + at,
                                         span);
        if (header <= 0) {
            return;
        }
        auto used = static_cast<std::size_t>(header);
        (void)core::log::append_hex(line, used, blob.subspan(start, span));
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), used});
    }
}

} // namespace

/** Collects the authored equipment and plug hashes every configured character names. */
bool collect_authored_hashes(AuthoredHashes& output) noexcept {
    output = {};
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account)) {
        return false;
    }
    const auto append = [&output](const state::account::inventory::Item& item) noexcept {
        if (output.count >= output.values.size()) {
            return false;
        }
        output.values[output.count++] = item.definitionHash;
        for (std::size_t lane = 0; lane < item.sockets.plugCount; ++lane) {
            if (!item.sockets.plugs[lane].has_value()) {
                continue;
            }
            if (output.count >= output.values.size()) {
                return false;
            }
            output.values[output.count++] = *item.sockets.plugs[lane];
        }
        return true;
    };
    for (std::size_t character = 0; character < account.characterCount; ++character) {
        for (const auto& item : account.characters[character].equipment.slots) {
            if (!item.has_value()) {
                continue;
            }
            if (!append(*item)) {
                return false;
            }
        }
        const state::account::inventory::CharacterItems& inventory =
            account.characters[character].inventory;
        for (std::size_t item = 0; item < inventory.count; ++item) {
            if (!append(inventory.values[item])) {
                return false;
            }
        }
    }
    const auto end = output.values.begin() + static_cast<std::ptrdiff_t>(output.count);
    std::sort(output.values.begin(), end);
    output.count =
        static_cast<std::size_t>(std::unique(output.values.begin(), end) - output.values.begin());
    return output.count != 0;
}

/** @param hashes Sorted authored hashes. @param hash Row hash. @return True when authored. */
bool authored(const AuthoredHashes& hashes, std::uint32_t hash) noexcept {
    const auto begin = hashes.values.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(hashes.count);
    return std::binary_search(begin, end, hash);
}

/** @return True when the row's bucket maps to a supported equipment slot. */
bool equippable(const tables::items::Row& row) noexcept {
    return row.equipmentSlot.has_value() || equipment_slot(row.bucketId).has_value();
}

/** Applies the bucket-definition equipment mapping and publishes the complete bucket table. */
bool publish_buckets(Storage& storage) noexcept {
    namespace buckets = state::build_data::inventory::buckets;
    if (state::build_data::inventory_bucket_descriptors_ready()) {
        return true;
    }
    if (storage.bucketCount == 0 || storage.bucketCount > storage.bucketRows.size()) {
        return false;
    }
    bool hasEquipmentSlot = false;
    for (std::size_t index = 0; index < storage.bucketCount; ++index) {
        buckets::Descriptor& descriptor = storage.bucketRows[index];
        descriptor.equipmentSlot = storage.equipmentSlotByBucket[descriptor.bucketId];
        hasEquipmentSlot =
            hasEquipmentSlot || descriptor.equipmentSlot != buckets::kUnavailableEquipmentSlot;
    }
    return hasEquipmentSlot
           && state::build_data::publish_inventory_bucket_descriptors(
               std::span(storage.bucketRows).first(storage.bucketCount));
}

/** Adds one definition index to the deduplicated requested set. */
void request(std::uint16_t definitionIndex, DetailRequests& requested) noexcept {
    if (static_cast<std::size_t>(definitionIndex) < requested.size()) {
        requested.set(definitionIndex);
    }
}

/** Adds every socket lane's initial plug to the requested set. */
void append_initial_plugs(const tables::items::Row& row,
                          std::uint64_t itemDefinitionCount,
                          DetailRequests& requested) noexcept {
    for (std::size_t lane = 0; lane < row.socketCount; ++lane) {
        if (row.initialPlugs[lane] == tables::items::kUnavailablePlug
            || row.initialPlugs[lane] >= itemDefinitionCount) {
            continue;
        }
        request(row.initialPlugs[lane], requested);
    }
}

/** Materializes requested native indices in ascending order. */
bool materialize_requests(const DetailRequests& requested,
                          std::span<std::uint16_t> output,
                          std::size_t& count) noexcept {
    count = 0;
    for (std::size_t index = 0; index < requested.size(); ++index) {
        if (!requested.test(index)) {
            continue;
        }
        if (count >= output.size()) {
            count = 0;
            return false;
        }
        output[count++] = static_cast<std::uint16_t>(index);
    }
    return true;
}

/** Reads one requested definition and turns it into its cached detail form. */
bool build_detail(const DetailSource& source,
                  std::uint16_t definitionIndex,
                  domain::Definition& detail,
                  tables::items::Row& item) noexcept {
    tables::IndexRow indexRow{};
    item = {};
    item.definitionIndex = definitionIndex;
    if (!tables::index_row(source.table, source.array, definitionIndex, indexRow)
        || !reader::read_tag(
            *source.source, *source.scratch, indexRow.targetTag, *source.definition)
        || !tables::items::read_definition(std::span<const std::byte>{*source.definition}, item)) {
        return false;
    }
    item.definitionHash = indexRow.definitionHash;
    detail = to_detail(item);
    return true;
}

/** Reads the stat rows the installed investment constants blob names. */
bool read_investment_constants(const reader::Source& source,
                               reader::Scratch& scratch,
                               std::span<const std::byte> root,
                               std::vector<std::byte>& blob,
                               state::build_data::constants::InvestmentConstants& output) noexcept {
    output = {};
    std::uint32_t tag = 0;
    if (!tables::slot_tag(root, tables::kInvestmentConstantsSlot, tag) || tag == 0
        || !reader::read_tag(source, scratch, tag, blob)) {
        return false;
    }
    const std::size_t last =
        kConstantsPrefix + kCharacterStatRowOffsets[std::size(kCharacterStatRowOffsets) - 1];
    if (blob.size() <= last) {
        return false;
    }
    output.lightStatRow =
        std::to_integer<std::uint8_t>(blob[kConstantsPrefix + kLightStatRowOffset]);
    for (std::size_t row = 0; row < std::size(kCharacterStatRowOffsets); ++row) {
        output.characterStatRows[row] =
            std::to_integer<std::uint8_t>(blob[kConstantsPrefix + kCharacterStatRowOffsets[row]]);
    }
    output.extracted = true;
    if (core::settings::get().client.slotSweep) {
        dump_investment_slots(source, scratch, root, blob);
    }
    if (core::settings::get().client.constantsDump) {
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=constants stage=rows size=%zu light=%u r0=%u r1=%u r2=%u r3=%u r4=%u r5=%u",
            blob.size(),
            static_cast<unsigned>(output.lightStatRow),
            static_cast<unsigned>(output.characterStatRows[0]),
            static_cast<unsigned>(output.characterStatRows[1]),
            static_cast<unsigned>(output.characterStatRows[2]),
            static_cast<unsigned>(output.characterStatRows[3]),
            static_cast<unsigned>(output.characterStatRows[4]),
            static_cast<unsigned>(output.characterStatRows[5]));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        dump_investment_constants(std::span<const std::byte>{blob});
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
