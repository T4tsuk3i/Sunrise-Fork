#include "activity_cutscene_route.h"

#include <atomic>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../build_data/scenarios/scenario_catalog.h"
#include "../destination/definition.h"

namespace sunrise::state::activity::cutscene {
namespace {

/**
 * The opening cutscene, as the installed packages name it. 110 is the campaign's first mission
 * and `twr` its Tower setting, which is where Homecoming opens.
 */
constexpr std::string_view kOpeningCutscene = "cine_110_twr";

/**
 * Bubble the cutscene is entered through. A cinematic scenario carries its camera work in its
 * first space, and none of the installed ones declare an authored arrival to say otherwise.
 */
constexpr std::uint8_t kArrivalBubble = 0;

/**
 * Slice-state index of that bubble. Each bubble owns eight consecutive states and the arrival
 * takes the first of its own, which is the same arithmetic the authored default destination uses.
 */
constexpr std::uint16_t kArrivalSliceSet = kArrivalBubble * defaults::kSliceStatesPerBubble;

/** Nothing owes a cutscene until a character is selected. */
std::atomic_bool g_armed{false};
std::atomic_uint64_t g_characterSoid{0};

/**
 * Builds the fallback policy from what the packages declare for this destination.
 * @param definition Extracted scenario layout.
 * @param fallback Receives the policy.
 */
void policy_from(const build_data::scenarios::Definition& definition,
                 defaults::FallbackPolicy& fallback) noexcept {
    fallback = {};
    fallback.bubbleCount = definition.bubbleCount;
    for (std::size_t bubble = 0; bubble < definition.bubbleCount; ++bubble) {
        if (definition.bubbleStates[bubble] == build_data::scenarios::kBubbleEnabledByte) {
            fallback.statefulBubbleMask |= std::uint64_t{1} << bubble;
        }
    }
    fallback.initialSliceSet = kArrivalSliceSet;
    // No set is named, so the client searches the loaded world for its own point. A set belongs
    // to the map rather than the bubble, and naming one the arrival bubble lacks spawns nothing.
    fallback.spawnSetHash = destination::kAbsentSpawnSetHash;
}

} // namespace

void arm(std::uint64_t characterSoid) noexcept {
    g_characterSoid.store(characterSoid, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_release);
    char buf[96]{};
    const int n = std::snprintf(buf,
                                sizeof buf,
                                "ev=cutscene stage=arm result=ok soid=0x%016llX",
                                static_cast<unsigned long long>(characterSoid));
    if (n > 0) {
        core::log::write(
            core::log::Channel::state, core::log::Level::info, {buf, static_cast<std::size_t>(n)});
    }
}

void disarm() noexcept {
    if (g_armed.exchange(false, std::memory_order_acq_rel)) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::info,
                         "ev=cutscene stage=disarm result=ok");
    }
}

bool armed() noexcept {
    return g_armed.load(std::memory_order_acquire);
}

bool apply(defaults::ActivityDefaults& defaults) noexcept {
    if (!armed()) {
        return false;
    }
    build_data::scenarios::Definition definition{};
    if (!build_data::scenarios::find(kOpeningCutscene, definition)
        || definition.bubbleCount < defaults::kMinimumBubbleCount) {
        // The packages do not carry it, so the authored default stands rather than a layout no
        // map can satisfy. Reported once per load, not once per snapshot, by clearing the debt.
        disarm();
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         "ev=cutscene stage=resolve result=fail reason=absent");
        return false;
    }

    destination::DestinationSelection& selection = defaults.defaultDestination.selection;
    selection = {};
    for (std::size_t index = 0; index < kOpeningCutscene.size(); ++index) {
        selection.packageName[index] = static_cast<std::int8_t>(kOpeningCutscene[index]);
    }
    selection.packageNameLength = static_cast<std::uint8_t>(kOpeningCutscene.size());
    // Named rather than picked, so the indices the client would have supplied are all absent.
    selection.reason = destination::kMinimumReason;
    selection.sourceActivityIndex = destination::kAbsentActivityIndex;
    selection.activityIndex = destination::kAbsentActivityIndex;
    selection.elementIndex = destination::kAbsentElementIndex;
    selection.arrivalBubbleOverride = kArrivalBubble;
    selection.hasArrivalBubbleOverride = true;
    policy_from(definition, defaults.defaultDestination.fallback);

    char buf[128]{};
    const int n = std::snprintf(buf,
                                sizeof buf,
                                "ev=cutscene stage=apply result=ok name=%.*s bubbles=%u slice=%u",
                                static_cast<int>(kOpeningCutscene.size()),
                                kOpeningCutscene.data(),
                                static_cast<unsigned>(definition.bubbleCount),
                                static_cast<unsigned>(kArrivalSliceSet));
    if (n > 0) {
        core::log::write(
            core::log::Channel::state, core::log::Level::info, {buf, static_cast<std::size_t>(n)});
    }
    return true;
}

} // namespace sunrise::state::activity::cutscene
