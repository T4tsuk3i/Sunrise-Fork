#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../content/content_catalog.h"
#include "abilities/ability_bucket_catalog.h"
#include "cache/internal.h"
#include "collectibles/collectible_catalog.h"
#include "constants/investment_constant_catalog.h"
#include "hash_names/hash_name_catalog.h"
#include "inventory/buckets/inventory_bucket_catalog.h"
#include "items/details/item_detail_catalog.h"
#include "items/socket_plugs/socket_plug_catalog.h"
#include "material_requirements/material_requirement_catalog.h"
#include "progressions/progression_catalog.h"
#include "runtime.h"
#include "runtime/build_data_catalog_runtime.h"
#include "runtime/domain_markers.h"
#include "runtime/persistence/build_data_persistence.h"
#include "scenarios/scenario_catalog.h"
#include "socket_entry_lists/socket_entry_list_catalog.h"
#include "spawn_sets/spawn_set_catalog.h"
#include "vendors/vendor_catalog.h"

namespace sunrise::state::build_data {
namespace {

/** One cache directory holds the generated build data under the artifact root. */
constexpr std::wstring_view kCacheDirectorySuffix = L"\\cache";
/** One reusable file stores all extracted build mappings. */
constexpr std::wstring_view kCacheFileSuffix = L"\\cache\\build_data.bin";

/**
 * Logs which step of applying a loaded cache to the in-memory catalogs failed. This whole path
 * previously returned false on any rejection with no logging anywhere, which turned a single bad
 * domain into a silent, unexplained boot failure.
 * @param reason Short key naming the step that failed.
 * @return False, for a direct return.
 */
[[nodiscard]] bool report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=build_data_runtime stage=%s result=fail", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Loads one full cache, or leaves every domain ready for first-boot extraction. */
bool initialize(void* module, std::uint64_t configuredEquipmentHash) noexcept {
    runtime::persistence::Context& persistenceState = runtime::persistence::context();
    AcquireSRWLockExclusive(&persistenceState.lock);
    runtime::persistence::clear_locked(persistenceState);
    runtime::clear_catalogs();
    if (module == nullptr) {
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return true;
    }

    core::path::Buffer moduleDirectory;
    if (!core::path::artifact_directory(module, moduleDirectory)) {
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return report_fail("artifact_directory");
    }
    persistenceState.cacheDirectory = moduleDirectory;
    persistenceState.cachePath = moduleDirectory;
    if (!core::path::append(persistenceState.cacheDirectory, kCacheDirectorySuffix)) {
        runtime::persistence::clear_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return report_fail("cache_directory_path");
    }
    if (!core::path::append(persistenceState.cachePath, kCacheFileSuffix)) {
        runtime::persistence::clear_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return report_fail("cache_file_path");
    }
    if (!cache::current_build_identity(configuredEquipmentHash, persistenceState.buildIdentity)) {
        runtime::persistence::clear_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return report_fail("build_identity");
    }
    persistenceState.enabled = true;

    cache::records::DomainCounts counts{};
    const cache::LoadStatus status =
        cache::load(persistenceState.cachePath.chars.data(),
                    persistenceState.buildIdentity,
                    runtime::persistence::scratch_domains(persistenceState),
                    counts);
    if (status == cache::LoadStatus::missing) {
        runtime::persistence::release_scratch_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return true;
    }
    if (status == cache::LoadStatus::stale || status == cache::LoadStatus::invalid) {
        // A stale cache is replaced only after every domain is complete. A cache the reader
        // rejects as malformed (built by an incompatible version of this codebase, truncated,
        // etc.) gets the same treatment: it is a derived, regenerable artifact, not save data,
        // so there is nothing to salvage from it and no reason boot should ever abort over it.
        persistenceState.replaceStaleCache = true;
        runtime::persistence::release_scratch_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return true;
    }
    const cache::records::Domains domains =
        runtime::persistence::occupied_domains(persistenceState, counts);
    bool detailsReplaced = false;
    if (domains.itemDetails.empty()) {
        items::details::clear();
        detailsReplaced = true;
    } else {
        detailsReplaced = items::details::replace(domains.itemDetails);
    }
    const constants::InvestmentConstants cachedConstants{
        domains.constants.extracted != 0,
        domains.constants.lightStatRow,
        domains.constants.characterStatRows,
    };
    const char* cacheApplyFailure = nullptr;
    if (status != cache::LoadStatus::loaded) {
        cacheApplyFailure = "status_not_loaded";
    } else if (!constants::replace(cachedConstants)) {
        cacheApplyFailure = "constants";
    } else if (!content::replace(domains.named)) {
        cacheApplyFailure = "content_named";
    } else if (!content::seal()) {
        cacheApplyFailure = "content_seal";
    } else if (!items::replace(domains.items)) {
        cacheApplyFailure = "items";
    } else if (!collectibles::replace(domains.collectibles)) {
        cacheApplyFailure = "collectibles";
    } else if (!material_requirements::replace(domains.materialRequirementSets)) {
        cacheApplyFailure = "material_requirements";
    } else if (!inventory::buckets::replace(domains.inventoryBuckets)) {
        cacheApplyFailure = "inventory_buckets";
    } else if (!socket_entry_lists::replace(domains.socketEntryLists)) {
        cacheApplyFailure = "socket_entry_lists";
    } else if (!socket_entry_lists::replace_entry_tables(domains.socketEntryTables)) {
        // The per-entry tables are what the subclass selection reads. Without them a cache hit
        // makes the lists ready, the package build skips itself, and no ability is picked.
        cacheApplyFailure = "socket_entry_tables";
    } else if (!detailsReplaced) {
        cacheApplyFailure = "item_details";
    } else if (!items::socket_plugs::replace(
                   domains.socketPlugRules, domains.socketPlugPools, domains.socketPlugMembers)) {
        cacheApplyFailure = "socket_plugs";
    } else if (!abilities::replace(domains.abilityBuckets)) {
        cacheApplyFailure = "ability_buckets";
    } else if (!progressions::replace(domains.progressions)) {
        cacheApplyFailure = "progressions";
    } else if (!scenarios::replace(domains.scenarios, domains.rosterGroups)) {
        // The layouts are what activity message 1 reads. Without them a cache hit makes the
        // other domains ready, the package build skips itself, and every destination falls back.
        cacheApplyFailure = "scenarios";
    } else if (!domains.spawnStems.empty()
               && !spawn_sets::replace(
                   domains.spawnStems, domains.spawnNameHashes, domains.spawnPoints)) {
        // An empty catalog is complete, so the spawn-set replace is skipped rather than failed.
        cacheApplyFailure = "spawn_sets";
    } else if (!domains.vendorIndex.empty()
               && !vendors::replace(domains.vendorIndex,
                                    domains.vendorDefinitions,
                                    domains.vendorSaleRows,
                                    domains.vendorInstalledRows)) {
        // An empty catalog is complete, so the vendor replace is skipped rather than failed.
        cacheApplyFailure = "vendors";
    } else if (!hash_names::replace(domains.hashNames)) {
        cacheApplyFailure = "hash_names";
    }
    if (cacheApplyFailure != nullptr) {
        // No domain remains published when any catalog rejects the cache transaction.
        runtime::clear_catalogs();
        runtime::persistence::clear_locked(persistenceState);
        ReleaseSRWLockExclusive(&persistenceState.lock);
        return report_fail(cacheApplyFailure);
    }
    runtime::details::publish();
    runtime::named::publish();
    runtime::ability_buckets::publish();
    runtime::spawn_catalog::publish();
    runtime::name_catalog::publish();
    persistenceState.persisted = true;
    runtime::persistence::release_scratch_locked(persistenceState);
    ReleaseSRWLockExclusive(&persistenceState.lock);
    return true;
}

/** Clears all generated build mappings and persistence paths. */
void shutdown() noexcept {
    runtime::persistence::Context& persistenceState = runtime::persistence::context();
    AcquireSRWLockExclusive(&persistenceState.lock);
    runtime::clear_catalogs();
    runtime::persistence::clear_locked(persistenceState);
    ReleaseSRWLockExclusive(&persistenceState.lock);
}

} // namespace sunrise::state::build_data
