#include <algorithm>
#include <atomic>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::middleware::datagen::character_record::appearance {
namespace {

/**
 * Socket types in priority order, most load-bearing first.
 * A loadout has more distinct plug hashes than the overflow bank holds, so this order decides
 * which ones survive. Each entry is one native socket type index.
 */
constexpr std::uint16_t kOverflowPriority[]{
    176, // weapon intrinsic frame
    677, // armour intrinsic
    61,  // sparrow drive
    92,  // weapon trait
    62,  // sparrow perks
    65,  // weapon barrel
    67,  // weapon blade
    159, // weapon guard
    177, // weapon magazine
    179, // weapon scope
    26,  // armour shader
    60,  // sparrow shader
    180, // weapon shader
};

/** A socket type outside the priority table ranks behind every type inside it. */
constexpr std::size_t kUnrankedPriority = std::size(kOverflowPriority);
/** Every equipped item can contribute one plug hash per socket lane. */
constexpr std::size_t kRankedCapacity =
    family4::loadout::kItemCapacity * details::kInitialPlugCapacity;

/** One plug hash with the three fields its rank is ordered by. */
struct Ranked {
    std::size_t priority{kUnrankedPriority};
    std::uint16_t definitionIndex{};
    std::uint16_t lane{};
    std::uint32_t definitionHash{};
};

/** @param socketType Native socket type. @return Its rank, or the unranked rank. */
[[nodiscard]] std::size_t priority_of(std::uint16_t socketType) noexcept {
    for (std::size_t rank = 0; rank < kUnrankedPriority; ++rank) {
        if (kOverflowPriority[rank] == socketType) {
            return rank;
        }
    }
    return kUnrankedPriority;
}

/** @param left Ranked hash. @param right Ranked hash. @return Priority, item, then lane order. */
[[nodiscard]] bool ranked_less(const Ranked& left, const Ranked& right) noexcept {
    if (left.priority != right.priority) {
        return left.priority < right.priority;
    }
    if (left.definitionIndex != right.definitionIndex) {
        return left.definitionIndex < right.definitionIndex;
    }
    return left.lane < right.lane;
}

/**
 * Appends one definition's sandbox perks to a bank.
 * @param definitionIndex Native item or plug index.
 * @param count Occupied entries, advanced per appended perk.
 */
void append_perks(std::uint16_t definitionIndex,
                  std::span<std::uint16_t> bank,
                  std::size_t& count) noexcept {
    details::Definition detail{};
    if (definitionIndex == details::kUnavailableItemIndex
        || !state::build_data::find_configured_item_detail(definitionIndex, detail)) {
        return;
    }
    const std::size_t perks = detail.sandboxPerkCount < detail.sandboxPerks.size()
                                  ? detail.sandboxPerkCount
                                  : detail.sandboxPerks.size();
    for (std::size_t entry = 0; entry < perks && count < bank.size(); ++entry) {
        bank[count++] = detail.sandboxPerks[entry];
    }
}

/**
 * Appends one equipped item's own perks followed by every plug lane's.
 * @param equipped Effective plug lanes.
 * @param count Occupied entries, advanced per appended perk.
 */
void append_item_perks(const Equipped& equipped,
                       std::span<std::uint16_t> bank,
                       std::size_t& count) noexcept {
    append_perks(equipped.definitionIndex, bank, count);
    for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
        append_perks(equipped.plugs[lane], bank, count);
    }
}

/** @param slot Native equipment slot. @return True when a per-weapon bank owns that slot. */
[[nodiscard]] bool weapon_slot(std::uint8_t slot) noexcept {
    return slot >= kFirstWeaponSlot
           && slot < kFirstWeaponSlot + static_cast<std::int8_t>(kWeaponSlotCount);
}

/** @param bank Hash bank. @return Entries not at the no-hash sentinel. */
[[nodiscard]] std::size_t
count_hashes_filled(const std::array<std::uint32_t, layout::kOverflowHashCapacity>& bank) noexcept {
    std::size_t count = 0;
    for (const std::uint32_t hash : bank) {
        if (hash != layout::kNoHash) {
            ++count;
        }
    }
    return count;
}

/** @param bank Definition-index bank. @return Entries not at the empty-index sentinel. */
template <std::size_t N>
[[nodiscard]] std::size_t
count_indices_filled(const std::array<std::uint16_t, N>& bank) noexcept {
    std::size_t count = 0;
    for (const std::uint16_t index : bank) {
        if (index != layout::kEmptyDefinitionIndex) {
            ++count;
        }
    }
    return count;
}

/** Diagnostic latch: the appearance block is fixed for one encode, so the first one settles it. */
std::atomic<bool> g_reportedEncoded{};

/**
 * Census reports allowed per run.
 * The census follows gear changes rather than latching once, because an exotic is equipped in the
 * world long after character select and a one-shot report never sees it. A swap is rare enough
 * that a small cap covers a session without the encode path being able to flood the log.
 */
constexpr std::size_t kMaxCensusReports = 12;
/** Fingerprint seed and multiplier, the 64-bit FNV-1a pair. */
constexpr std::uint64_t kFingerprintSeed = 14695981039346656037ULL;
/** Multiplier paired with the seed above. */
constexpr std::uint64_t kFingerprintPrime = 1099511628211ULL;
/** Line buffer for one census row. An item's lanes and their perks are the longest line here. */
constexpr std::size_t kCensusLineCapacity = 512;
/** Bytes kept free while appending one ` p=%u` or ` l%zu:%u` token, which cannot exceed this. */
constexpr std::size_t kCensusTokenReserve = 24;
/** Perk-bank entries named per line. The character bank is the only one long enough to truncate. */
constexpr std::size_t kCensusBankEntries = 32;

/**
 * Appends one definition's sandbox perk indices to a census line.
 * The bank stores perk definition indices, so these are the exact values `append_perks` would
 * push: a perk named here and absent from the shipped bank was dropped by a capacity limit.
 * @param definitionIndex Native item or plug index, or the unavailable sentinel.
 * @param prefix Token prefix naming the source, `p` for the item itself and `l<lane>` for a plug.
 * @param lane Lane the plug sits in, ignored when the source is the item itself.
 * @param line Census line being built.
 * @param used Bytes already written, advanced per appended token.
 */
void append_census_perks(std::uint16_t definitionIndex,
                         std::string_view prefix,
                         std::size_t lane,
                         std::span<char> line,
                         std::size_t& used) noexcept {
    details::Definition detail{};
    if (definitionIndex == details::kUnavailableItemIndex
        || !state::build_data::find_configured_item_detail(definitionIndex, detail)) {
        return;
    }
    const std::size_t perks = detail.sandboxPerkCount < detail.sandboxPerks.size()
                                  ? detail.sandboxPerkCount
                                  : detail.sandboxPerks.size();
    for (std::size_t entry = 0; entry < perks && used + kCensusTokenReserve < line.size();
         ++entry) {
        const int extra = std::snprintf(line.data() + used,
                                        line.size() - used,
                                        " %.*s%zu=%u",
                                        static_cast<int>(prefix.size()),
                                        prefix.data(),
                                        lane,
                                        static_cast<unsigned>(detail.sandboxPerks[entry]));
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
}

/**
 * Fingerprints the equipped set by definition index and socketed plug.
 * Equality of this value means the same items carrying the same plugs, which is exactly when a
 * repeat census would say what the previous one already said.
 * @param instances Resolved equipped set.
 * @return Order-dependent digest of the set.
 */
[[nodiscard]] std::uint64_t equipped_fingerprint(
    const family4::loadout::ResolvedInstances& instances) noexcept {
    std::uint64_t digest = kFingerprintSeed;
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        details::Definition detail{};
        Equipped equipped{};
        if (!resolve_equipped(instances.items[index], detail, equipped)) {
            continue;
        }
        digest = (digest ^ equipped.definitionIndex) * kFingerprintPrime;
        for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
            digest = (digest ^ equipped.plugs[lane]) * kFingerprintPrime;
        }
    }
    return digest;
}

/** Equipped set the last census described, so an unchanged loadout is not reported twice. */
std::atomic<std::uint64_t> g_censusFingerprint{};
/** Censuses emitted so far, against kMaxCensusReports. */
std::atomic<std::size_t> g_censusReports{};

/**
 * Reports every sandbox perk each equipped item and plug contributes.
 * An exotic's effect travels as a sandbox perk on the item or on its intrinsic plug, so this names
 * the perk indices an equipped exotic actually put into the record. Pairing it with a stopwatch on
 * the ability is what decides whether the client acts on the perks it is handed.
 * @param soid Character the record belongs to.
 * @param instances Resolved equipped set.
 */
void report_perk_census(std::uint64_t soid,
                        const family4::loadout::ResolvedInstances& instances) noexcept {
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        details::Definition detail{};
        Equipped equipped{};
        if (!resolve_equipped(instances.items[index], detail, equipped)) {
            continue;
        }
        std::array<char, kCensusLineCapacity> line{};
        const int header = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=perk_census stage=item soid=0x%llX slot=%u def=%u "
                                         "hash=0x%08X lanes=%zu",
                                         static_cast<unsigned long long>(soid),
                                         static_cast<unsigned>(equipped.equipmentSlot),
                                         static_cast<unsigned>(equipped.definitionIndex),
                                         detail.definitionHash,
                                         equipped.laneCount);
        if (header <= 0) {
            continue;
        }
        auto used = static_cast<std::size_t>(header);
        append_census_perks(equipped.definitionIndex, "p", 0, line, used);
        for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
            append_census_perks(equipped.plugs[lane], "l", lane, line, used);
        }
        core::log::write(core::log::Channel::middleware, core::log::Level::info, {line.data(), used});
    }
}

/**
 * Reports the leading entries of the shipped character perk bank.
 * The fill counts alone cannot answer whether one particular perk survived into the record, and
 * that is the whole question when an exotic is being tested.
 * @param soid Character the record belongs to.
 * @param appearance Completed appearance block, all fills applied.
 */
void report_perk_bank_entries(std::uint64_t soid,
                              const layout::Appearance& appearance) noexcept {
    std::array<char, kCensusLineCapacity> line{};
    const int header = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=perk_census stage=bank soid=0x%llX filled=%zu",
                                     static_cast<unsigned long long>(soid),
                                     count_indices_filled(appearance.indexBank));
    if (header <= 0) {
        return;
    }
    auto used = static_cast<std::size_t>(header);
    const std::size_t entries = appearance.indexBank.size() < kCensusBankEntries
                                    ? appearance.indexBank.size()
                                    : kCensusBankEntries;
    for (std::size_t entry = 0; entry < entries && used + kCensusTokenReserve < line.size();
         ++entry) {
        if (appearance.indexBank[entry] == layout::kEmptyDefinitionIndex) {
            continue;
        }
        const int extra = std::snprintf(line.data() + used,
                                        line.size() - used,
                                        " i%zu=%u",
                                        entry,
                                        static_cast<unsigned>(appearance.indexBank[entry]));
        if (extra > 0) {
            used += static_cast<std::size_t>(extra);
        }
    }
    core::log::write(core::log::Channel::middleware, core::log::Level::info, {line.data(), used});
}

} // namespace

/**
 * Reports the completed appearance block's ability buckets, overflow bank, and perk-bank fills once.
 * Unlike the declared-side ability probe, this runs after every fill and reports the actual wire
 * content: bucket kinds and per-bucket hash counts, the overflow bank's filled count, and how many
 * definition indices each perk bank carries. Together with character_appearance_abilities.cpp's
 * probe this separates "the catalog is empty" from "the record is empty".
 * @param soid Character the record belongs to, so a three-character encode names its subject.
 * @param instances Resolved equipped set the banks were filled from, for perk attribution.
 * @param appearance Completed appearance block, all fills applied.
 */
void report_encoded_probe(std::uint64_t soid,
                          const family4::loadout::ResolvedInstances& instances,
                          const layout::Appearance& appearance) noexcept {
    // Ahead of the one-shot latch on purpose. The bank and bucket lines describe a record that is
    // fixed for the run, but an exotic is equipped in the world long after character select, so a
    // census that latched with them would only ever describe the loadout the player started in.
    if (core::settings::get().client.perkCensus) {
        const std::uint64_t fingerprint = equipped_fingerprint(instances);
        if (g_censusFingerprint.exchange(fingerprint, std::memory_order_relaxed) != fingerprint
            && g_censusReports.fetch_add(1, std::memory_order_relaxed) < kMaxCensusReports) {
            report_perk_census(soid, instances);
            report_perk_bank_entries(soid, appearance);
        }
    }
    if (g_reportedEncoded.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    {
        std::array<char, 256> kinds{};
        const int header = std::snprintf(kinds.data(),
                                         kinds.size(),
                                         "ev=bank stage=kinds soid=0x%llX",
                                         static_cast<unsigned long long>(soid));
        auto used = header > 0 ? static_cast<std::size_t>(header) : std::size_t{};
        for (std::size_t bucket = 0;
             bucket < appearance.abilityBuckets.size() && used + 16 < kinds.size();
             ++bucket) {
            const int extra = std::snprintf(kinds.data() + used,
                                            kinds.size() - used,
                                            " b%zu=%d",
                                            bucket,
                                            static_cast<int>(appearance.abilityBuckets[bucket].kind));
            if (extra > 0) {
                used += static_cast<std::size_t>(extra);
            }
        }
        if (used > 0) {
            core::log::write(core::log::Channel::middleware, core::log::Level::info,
                             {kinds.data(), used});
        }
    }
    {
        std::array<char, 384> fills{};
        const int header = std::snprintf(fills.data(),
                                         fills.size(),
                                         "ev=bank stage=bucket_fills overflow=%zu",
                                         count_hashes_filled(appearance.overflowHashes));
        auto used = header > 0 ? static_cast<std::size_t>(header) : std::size_t{};
        for (std::size_t bucket = 0;
             bucket < appearance.abilityBuckets.size() && used + 16 < fills.size();
             ++bucket) {
            std::size_t count = 0;
            for (const std::uint32_t hash : appearance.abilityBuckets[bucket].hashes) {
                if (hash != layout::kNoHash) {
                    ++count;
                }
            }
            const int extra = std::snprintf(
                fills.data() + used, fills.size() - used, " b%zu=%zu", bucket, count);
            if (extra > 0) {
                used += static_cast<std::size_t>(extra);
            }
        }
        if (used > 0) {
            core::log::write(core::log::Channel::middleware,
                             core::log::Level::info,
                             {fills.data(), used});
        }
    }
    {
        std::array<char, 384> banks{};
        const std::size_t character = count_indices_filled(appearance.indexBank);
        const std::size_t weaponA = count_indices_filled(appearance.smallBankA);
        const std::size_t weaponB = count_indices_filled(appearance.smallBankB);
        const std::size_t weaponC = count_indices_filled(appearance.smallBankC);
        const int n = std::snprintf(banks.data(),
                                    banks.size(),
                                    "ev=bank stage=index character=%zu weapon_a=%zu weapon_b=%zu "
                                    "weapon_c=%zu i0=%u i1=%u ia0=%u ib0=%u ic0=%u",
                                    character,
                                    weaponA,
                                    weaponB,
                                    weaponC,
                                    static_cast<unsigned>(appearance.indexBank[0]),
                                    static_cast<unsigned>(appearance.indexBank[1]),
                                    static_cast<unsigned>(appearance.smallBankA[0]),
                                    static_cast<unsigned>(appearance.smallBankB[0]),
                                    static_cast<unsigned>(appearance.smallBankC[0]));
        if (n > 0) {
            core::log::write(core::log::Channel::middleware,
                             core::log::Level::info,
                             {banks.data(), static_cast<std::size_t>(n)});
        }
    }
}

/** Fills the overflow hash bank from every equipped socket plug, in socket-type priority order. */
void apply_overflow_hashes(const family4::loadout::ResolvedInstances& instances,
                           layout::Appearance& appearance) noexcept {
    std::array<Ranked, kRankedCapacity> ranked{};
    std::size_t rankedCount = 0;
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        details::Definition detail{};
        Equipped equipped{};
        if (!resolve_equipped(instances.items[index], detail, equipped)) {
            continue;
        }
        for (std::size_t lane = 0; lane < equipped.laneCount && rankedCount < ranked.size();
             ++lane) {
            details::Definition plug{};
            if (equipped.plugs[lane] == details::kUnavailableItemIndex
                || !state::build_data::find_configured_item_detail(equipped.plugs[lane], plug)) {
                continue;
            }
            ranked[rankedCount++] = {priority_of(detail.socketTypes[lane]),
                                     equipped.definitionIndex,
                                     static_cast<std::uint16_t>(lane),
                                     plug.definitionHash};
        }
    }
    std::sort(
        ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(rankedCount), ranked_less);
    std::array<std::uint32_t, kRankedCapacity> unique{};
    std::size_t uniqueCount = 0;
    for (std::size_t entry = 0; entry < rankedCount; ++entry) {
        const std::uint32_t hash = ranked[entry].definitionHash;
        const auto end = unique.begin() + static_cast<std::ptrdiff_t>(uniqueCount);
        if (std::find(unique.begin(), end, hash) == end) {
            unique[uniqueCount++] = hash;
        }
    }
    // Only slots the subclass left at the sentinel are free, so the two fills never collide.
    std::size_t taken = 0;
    for (std::uint32_t& slot : appearance.overflowHashes) {
        if (taken >= uniqueCount) {
            return;
        }
        if (slot == layout::kNoHash) {
            slot = unique[taken++];
        }
    }
}

/** Fills the character-wide and the three per-weapon sandbox perk banks. */
void apply_perk_banks(const family4::loadout::ResolvedInstances& instances,
                      layout::Appearance& appearance) noexcept {
    const std::array<std::span<std::uint16_t>, kWeaponSlotCount> weaponBanks{
        appearance.smallBankA, appearance.smallBankB, appearance.smallBankC};
    std::size_t characterCount = 0;
    // Slot order is the character bank's own order, so the loop visits slots rather than the
    // instance list, whose order is the loadout's.
    for (std::uint8_t slot = 0; slot < layout::kRenderSlotCapacity; ++slot) {
        for (std::size_t index = 0; index < instances.itemCount; ++index) {
            if (instances.items[index].equipmentSlot != slot) {
                continue;
            }
            details::Definition detail{};
            Equipped equipped{};
            if (!resolve_equipped(instances.items[index], detail, equipped)) {
                continue;
            }
            if (weapon_slot(slot)) {
                std::size_t weaponCount = 0;
                append_item_perks(equipped,
                                  weaponBanks[static_cast<std::size_t>(slot - kFirstWeaponSlot)],
                                  weaponCount);
                continue;
            }
            append_item_perks(equipped, appearance.indexBank, characterCount);
        }
    }
}

} // namespace sunrise::middleware::datagen::character_record::appearance
