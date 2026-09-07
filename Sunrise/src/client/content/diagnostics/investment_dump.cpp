#include "investment_dump.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/items/details/definition.h"
#include "../../../state/build_data/items/item_catalog.h"
#include "../../../state/build_data/items/socket_plugs/definition.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::client::content::diagnostics {
namespace {

/** One dump line never exceeds this. */
constexpr std::size_t kLineCapacity = 192;
/** Bucket ids are a byte, and the all-set id names no bucket. */
constexpr std::size_t kBucketIdLimit = 255;
/** Sample hashes carried per bucket, enough to identify what the bucket holds. */
constexpr std::size_t kSampleCapacity = 4;

std::atomic<bool> g_dumped{};

/** Writes one prepared dump line. */
void emit(std::string_view line) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::info, line);
}

/** Lists every inventory bucket the installed build declares, equipment-mapped or not. */
void dump_buckets() noexcept {
    for (std::size_t id = 0; id < kBucketIdLimit; ++id) {
        state::build_data::inventory::buckets::Descriptor descriptor{};
        if (!state::build_data::find_inventory_bucket_descriptor(static_cast<std::uint8_t>(id),
                                                                 descriptor)) {
            continue;
        }
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=dump stage=buckets id=%zu selector=%u first=%u "
                                          "slots=%u equip=%d",
                                          id,
                                          static_cast<unsigned>(descriptor.arraySelector),
                                          static_cast<unsigned>(descriptor.firstSlot),
                                          static_cast<unsigned>(descriptor.slotCount),
                                          static_cast<int>(descriptor.equipmentSlot));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Lists which definition index each progression slot of one scope carries. */
void dump_progression_scope(state::build_data::progressions::Scope scope,
                            const char* scopeName) noexcept {
    static std::array<std::uint16_t, state::build_data::progressions::kDefinitionCapacity> slots{};
    std::size_t count = 0;
    if (!state::build_data::find_progression_slots(scope, slots, count)) {
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=dump stage=prog_slots scope=%s result=fail",
                                          scopeName);
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }
    for (std::size_t slot = 0; slot < count; ++slot) {
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=dump stage=prog_slots scope=%s slot=%zu def=%u",
                                          scopeName,
                                          slot,
                                          static_cast<unsigned>(slots[slot]));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/**
 * Counts item rows per inventory bucket and carries a few hashes out of each.
 *
 * A bucket with no equipment slot is the interesting case: the artifact, if this build carries one,
 * lands in exactly that shape. The sample hashes are what identifies the bucket's contents against
 * a published manifest, since nothing in the build data names an item.
 */
void dump_bucket_items() noexcept {
    static std::array<std::uint32_t, kBucketIdLimit + 1> countByBucket{};
    static std::array<std::array<std::uint32_t, kSampleCapacity>, kBucketIdLimit + 1> samples{};
    countByBucket.fill(0);
    for (auto& bucket : samples) {
        bucket.fill(0);
    }
    const std::size_t rows = state::build_data::items::count();
    for (std::size_t index = 0; index < rows; ++index) {
        state::build_data::items::Definition definition{};
        if (!state::build_data::items::find_index(static_cast<std::uint16_t>(index), definition)) {
            continue;
        }
        const std::size_t bucket = definition.bucketId;
        if (countByBucket[bucket] < kSampleCapacity) {
            samples[bucket][countByBucket[bucket]] = definition.definitionHash;
        }
        ++countByBucket[bucket];
    }
    for (std::size_t bucket = 0; bucket <= kBucketIdLimit; ++bucket) {
        if (countByBucket[bucket] == 0) {
            continue;
        }
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=dump stage=bucket_items bucket=%zu items=%u "
                                          "h0=0x%08X h1=0x%08X h2=0x%08X h3=0x%08X",
                                          bucket,
                                          countByBucket[bucket],
                                          samples[bucket][0],
                                          samples[bucket][1],
                                          samples[bucket][2],
                                          samples[bucket][3]);
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** One (bucket, tier, lane count) combination seen, and how many definitions shared it. */
struct SocketShape {
    std::uint8_t bucket{};
    std::uint8_t tier{};
    std::uint8_t lanes{};
    std::uint32_t count{};
};

/** Bounded distinct (bucket, tier, lane count) combinations the sweep can hold. */
constexpr std::size_t kShapeCapacity = 256;
/** Pool members sampled per lane, enough to identify what a lane holds. */
constexpr std::size_t kPoolSampleCapacity = 3;

/** Context threaded through the plain-function-pointer pool visitor below. */
struct PoolSampleContext {
    std::array<std::uint16_t, kPoolSampleCapacity> members{};
    std::size_t count{};
    std::size_t total{};
};

/** Collects up to kPoolSampleCapacity members and keeps counting past that. */
bool collect_pool_sample(void* context, state::build_data::items::socket_plugs::Member member) noexcept {
    auto& sample = *static_cast<PoolSampleContext*>(context);
    if (sample.count < sample.members.size()) {
        sample.members[sample.count] = member;
        ++sample.count;
    }
    ++sample.total;
    return true;
}

/**
 * Groups every item with an ordinary-socket block by (bucket, tier, lane count).
 * A masterworked legendary piece should show a materially higher lane count than the rare gear
 * already confirmed to carry exactly two mod lanes; this finds any such shape without assuming
 * which bucket id armor landed in.
 */
void dump_socket_shapes() noexcept {
    static std::array<SocketShape, kShapeCapacity> shapes{};
    std::size_t shapeCount = 0;
    const std::size_t rows = state::build_data::items::count();
    for (std::size_t index = 0; index < rows; ++index) {
        state::build_data::items::Definition item{};
        state::build_data::items::details::Definition detail{};
        if (!state::build_data::items::find_index(static_cast<std::uint16_t>(index), item)
            || !state::build_data::find_configured_item_detail(static_cast<std::uint16_t>(index),
                                                                detail)
            || detail.ordinarySocketState
                   != state::build_data::items::details::OrdinarySocketState::present) {
            continue;
        }
        bool matched = false;
        for (std::size_t shapeIndex = 0; shapeIndex < shapeCount; ++shapeIndex) {
            SocketShape& shape = shapes[shapeIndex];
            if (shape.bucket == item.bucketId && shape.tier == item.tier
                && shape.lanes == detail.ordinarySocketCount) {
                ++shape.count;
                matched = true;
                break;
            }
        }
        if (!matched && shapeCount < shapes.size()) {
            shapes[shapeCount] = {item.bucketId, item.tier, detail.ordinarySocketCount, 1};
            ++shapeCount;
        }
    }
    for (std::size_t index = 0; index < shapeCount; ++index) {
        const SocketShape& shape = shapes[index];
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=dump stage=socket_shape bucket=%u tier=%u lanes=%u "
                                          "items=%u",
                                          static_cast<unsigned>(shape.bucket),
                                          static_cast<unsigned>(shape.tier),
                                          static_cast<unsigned>(shape.lanes),
                                          shape.count);
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Dumps one item's full per-lane socket detail: socket type, initial plug, and pool sample. */
void dump_item_lanes(std::uint16_t definitionIndex) noexcept {
    state::build_data::items::Definition item{};
    state::build_data::items::details::Definition detail{};
    if (!state::build_data::items::find_index(definitionIndex, item)
        || !state::build_data::find_configured_item_detail(definitionIndex, detail)) {
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=dump stage=item_lanes index=%u result=fail",
                                          static_cast<unsigned>(definitionIndex));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }
    {
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=dump stage=item_lanes index=%u hash=0x%08X "
                                          "bucket=%u tier=%u lanes=%u",
                                          static_cast<unsigned>(definitionIndex),
                                          item.definitionHash,
                                          static_cast<unsigned>(item.bucketId),
                                          static_cast<unsigned>(item.tier),
                                          static_cast<unsigned>(detail.ordinarySocketCount));
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
    }
    for (std::uint8_t lane = 0; lane < detail.ordinarySocketCount
                                && lane < state::build_data::items::details::kInitialPlugCapacity;
         ++lane) {
        PoolSampleContext sample{};
        const bool hasPool = state::build_data::visit_socket_plug_pool(
            definitionIndex, lane, &collect_pool_sample, &sample);
        std::array<char, kLineCapacity> line{};
        int written = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=dump stage=item_lane index=%u lane=%u type=%u "
                                    "initial=%u pool=%zu",
                                    static_cast<unsigned>(definitionIndex),
                                    static_cast<unsigned>(lane),
                                    static_cast<unsigned>(detail.socketTypes[lane]),
                                    static_cast<unsigned>(detail.initialPlugIndices[lane]),
                                    hasPool ? sample.total : 0);
        if (written > 0) {
            std::size_t used = static_cast<std::size_t>(written);
            for (std::size_t sampleIndex = 0;
                 sampleIndex < sample.count && used + 24 < line.size();
                 ++sampleIndex) {
                const int extra = std::snprintf(line.data() + used,
                                                line.size() - used,
                                                " m%zu=%u",
                                                sampleIndex,
                                                static_cast<unsigned>(sample.members[sampleIndex]));
                if (extra > 0) {
                    used += static_cast<std::size_t>(extra);
                }
            }
            emit({line.data(), used});
        }
    }
}

/** Distinct pools tracked per (bucket, role, lane) group before further ones are dropped. */
constexpr std::size_t kDistinctPoolCapacity = 8;
/** (bucket, role, lane) groups tracked across the whole sweep. */
constexpr std::size_t kModLaneGroupCapacity = 512;
/** Resolved lane role for a pool whose first member is an ornament; not a mod lane. */
constexpr std::uint8_t kOrnamentBucketId = 13;
/** Resolved lane role for a pool whose first member is a shader; not a mod lane. */
constexpr std::uint8_t kShaderBucketId = 14;
/**
 * Native ordinary socket type whose choices are the synthetic tracker/masterwork set.
 * Mirrors `package_socket_plug_build.cpp`'s own constant by value rather than by dependency, since
 * this diagnostics file has no reason to link against the packages-build translation unit.
 */
constexpr std::uint16_t kTrackerSocketType = 518;
/** FNV-1a constants, matching the packages-side pool fingerprint so folding is stable. */
constexpr std::uint64_t kHashOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

/** One distinct pool seen inside a (bucket, role, lane) group, identified by its fingerprint. */
struct ModLanePool {
    std::uint64_t fingerprint{};
    std::uint32_t memberCount{};
    std::uint32_t items{};
};

/** Every item sharing one (bucket, role, lane) combination, and the distinct pools they carry. */
struct ModLaneGroup {
    std::uint8_t bucket{};
    std::uint8_t role{};
    std::uint8_t lane{};
    std::uint32_t items{};
    std::array<ModLanePool, kDistinctPoolCapacity> pools{};
    std::size_t poolCount{};
};

/** Context folding one lane's pool into an order-stable fingerprint, plain-function-pointer safe. */
struct PoolFingerprintContext {
    std::uint64_t fingerprint{kHashOffsetBasis};
    std::uint16_t firstMember{};
    std::size_t count{};
};

/** Folds one pool member into the running fingerprint; pool members visit in sorted order. */
bool fold_pool_fingerprint(void* context,
                           state::build_data::items::socket_plugs::Member member) noexcept {
    auto& ctx = *static_cast<PoolFingerprintContext*>(context);
    if (ctx.count == 0) {
        ctx.firstMember = member;
    }
    std::uint16_t value = member;
    for (std::size_t byte = 0; byte < sizeof value; ++byte) {
        ctx.fingerprint ^= static_cast<std::uint8_t>(value);
        ctx.fingerprint *= kHashPrime;
        value >>= 8U;
    }
    ++ctx.count;
    return true;
}

/**
 * Groups every ordinary mod-like socket lane by (item bucket, lane's resolved plug role, lane
 * index), and counts how many distinct plug pools that group actually carries.
 *
 * A group with exactly one distinct pool means every item sharing that bucket/lane already accepts
 * the same plugs regardless of whatever else distinguishes those items (element included) -- there
 * is nothing to fix. A group with more than one distinct pool is the confirmed split a later merge
 * would need to target. Lane role, not a hardcoded item list, is what tells a mod lane apart from
 * an ornament or shader lane: the role is the bucket the lane's own first pool member resolves to.
 */
void dump_mod_lane_pools() noexcept {
    static std::array<ModLaneGroup, kModLaneGroupCapacity> groups{};
    std::size_t groupCount = 0;
    const std::size_t rows = state::build_data::items::count();
    for (std::size_t index = 0; index < rows; ++index) {
        state::build_data::items::Definition item{};
        state::build_data::items::details::Definition detail{};
        if (!state::build_data::items::find_index(static_cast<std::uint16_t>(index), item)
            || !state::build_data::find_configured_item_detail(static_cast<std::uint16_t>(index),
                                                                detail)
            || detail.ordinarySocketState
                   != state::build_data::items::details::OrdinarySocketState::present) {
            continue;
        }
        for (std::uint8_t lane = 0; lane < detail.ordinarySocketCount
                                    && lane < state::build_data::items::details::kInitialPlugCapacity;
             ++lane) {
            if (detail.socketTypes[lane]
                    == state::build_data::items::details::kUnavailableSocketType
                || detail.socketTypes[lane] == kTrackerSocketType) {
                continue;
            }
            PoolFingerprintContext fingerprint{};
            if (!state::build_data::visit_socket_plug_pool(
                    static_cast<std::uint16_t>(index), lane, &fold_pool_fingerprint, &fingerprint)
                || fingerprint.count == 0) {
                continue;
            }
            fingerprint.fingerprint ^= fingerprint.count;
            fingerprint.fingerprint *= kHashPrime;

            state::build_data::items::Definition firstMemberDefinition{};
            std::uint8_t role = state::build_data::items::kUnresolvedBucketId;
            if (state::build_data::items::find_index(fingerprint.firstMember,
                                                      firstMemberDefinition)) {
                role = firstMemberDefinition.bucketId;
            }
            if (role == kOrnamentBucketId || role == kShaderBucketId) {
                continue;
            }

            ModLaneGroup* group = nullptr;
            for (std::size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
                if (groups[groupIndex].bucket == item.bucketId && groups[groupIndex].role == role
                    && groups[groupIndex].lane == lane) {
                    group = &groups[groupIndex];
                    break;
                }
            }
            if (group == nullptr) {
                if (groupCount >= groups.size()) {
                    continue;
                }
                groups[groupCount] = {item.bucketId, role, lane, 0, {}, 0};
                group = &groups[groupCount];
                ++groupCount;
            }
            ++group->items;
            bool matchedPool = false;
            for (std::size_t poolIndex = 0; poolIndex < group->poolCount; ++poolIndex) {
                ModLanePool& pool = group->pools[poolIndex];
                if (pool.fingerprint == fingerprint.fingerprint
                    && pool.memberCount == fingerprint.count) {
                    ++pool.items;
                    matchedPool = true;
                    break;
                }
            }
            if (!matchedPool && group->poolCount < group->pools.size()) {
                group->pools[group->poolCount] = {fingerprint.fingerprint,
                                                  static_cast<std::uint32_t>(fingerprint.count),
                                                  1};
                ++group->poolCount;
            }
        }
    }
    for (std::size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        const ModLaneGroup& group = groups[groupIndex];
        std::array<char, kLineCapacity> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=dump stage=mod_lane_group bucket=%u role=%u lane=%u "
                          "distinct_pools=%zu items=%u",
                          static_cast<unsigned>(group.bucket),
                          static_cast<unsigned>(group.role),
                          static_cast<unsigned>(group.lane),
                          group.poolCount,
                          group.items);
        if (written > 0) {
            emit({line.data(), static_cast<std::size_t>(written)});
        }
        for (std::size_t poolIndex = 0; poolIndex < group.poolCount; ++poolIndex) {
            const ModLanePool& pool = group.pools[poolIndex];
            std::array<char, kLineCapacity> poolLine{};
            const int poolWritten =
                std::snprintf(poolLine.data(),
                              poolLine.size(),
                              "ev=dump stage=mod_lane_pool bucket=%u role=%u lane=%u "
                              "fingerprint=0x%016llX members=%u items=%u",
                              static_cast<unsigned>(group.bucket),
                              static_cast<unsigned>(group.role),
                              static_cast<unsigned>(group.lane),
                              static_cast<unsigned long long>(pool.fingerprint),
                              pool.memberCount,
                              pool.items);
            if (poolWritten > 0) {
                emit({poolLine.data(), static_cast<std::size_t>(poolWritten)});
            }
        }
    }
}

} // namespace

/** Dumps the bucket, progression and per-bucket item tables once, for offline identification. */
void dump_investment_tables() noexcept {
    if (!state::build_data::inventory_bucket_descriptors_ready()
        || !state::build_data::progression_definitions_ready()
        || !state::build_data::item_definitions_ready()) {
        return;
    }
    if (g_dumped.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    emit("ev=dump stage=begin");
    dump_buckets();
    dump_progression_scope(state::build_data::progressions::Scope::account, "account");
    dump_progression_scope(state::build_data::progressions::Scope::character, "character");
    dump_bucket_items();
    emit("ev=dump stage=end");
}

/** Dumps every armor definition's ordinary-socket lanes and each lane's plug pool, once. */
void dump_armor_sockets() noexcept {
    if (!state::build_data::configured_item_details_ready()
        || !state::build_data::socket_plug_rules_ready()
        || !state::build_data::item_definitions_ready()) {
        return;
    }
    static std::atomic<bool> dumped{};
    if (dumped.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    emit("ev=dump stage=armor_begin");
    dump_socket_shapes();
    // The exact pieces equipped and plugged tonight, taken from tonight's own socket_plug log
    // lines, so this reads the lanes actually exercised rather than a guessed sample.
    constexpr std::array<std::uint16_t, 6> kEquippedThisSession{1107, 1108, 1109, 1110, 1111, 10021};
    for (const std::uint16_t index : kEquippedThisSession) {
        dump_item_lanes(index);
    }
    emit("ev=dump stage=armor_end");
}

/** Dumps every mod-like socket lane's distinct plug pools, grouped by (bucket, role, lane), once. */
void dump_mod_lane_pools_once() noexcept {
    if (!state::build_data::configured_item_details_ready()
        || !state::build_data::socket_plug_rules_ready()
        || !state::build_data::item_definitions_ready()) {
        return;
    }
    static std::atomic<bool> dumped{};
    if (dumped.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    emit("ev=dump stage=mod_lanes_begin");
    dump_mod_lane_pools();
    emit("ev=dump stage=mod_lanes_end");
}

} // namespace sunrise::client::content::diagnostics
