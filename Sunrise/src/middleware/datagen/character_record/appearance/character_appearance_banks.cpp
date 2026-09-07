#include <algorithm>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
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

} // namespace

/**
 * Reports the completed appearance block's ability buckets, overflow bank, and perk-bank fills once.
 * Unlike the declared-side ability probe, this runs after every fill and reports the actual wire
 * content: bucket kinds and per-bucket hash counts, the overflow bank's filled count, and how many
 * definition indices each perk bank carries. Together with character_appearance_abilities.cpp's
 * probe this separates "the catalog is empty" from "the record is empty".
 * @param appearance Completed appearance block, all fills applied.
 */
void report_encoded_probe(std::uint64_t soid, const layout::Appearance& appearance) noexcept {
    if (g_reportedEncoded.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    {
        std::array<char, 256> kinds{};
        int used = std::snprintf(kinds.data(),
                                 kinds.size(),
                                 "ev=bank stage=kinds soid=0x%llX",
                                 static_cast<unsigned long long>(soid));
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
            core::log::write(core::log::Channel::middleware,
                             core::log::Level::info,
                             {kinds.data(), static_cast<std::size_t>(used)});
        }
    }
    {
        std::array<char, 384> fills{};
        int used = std::snprintf(fills.data(),
                                 fills.size(),
                                 "ev=bank stage=bucket_fills overflow=%zu",
                                 count_hashes_filled(appearance.overflowHashes));
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
                             {fills.data(), static_cast<std::size_t>(used)});
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
