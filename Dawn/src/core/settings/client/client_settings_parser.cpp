#include "../parser.h"

namespace dawn::core::settings::parser {

/** Parses Client-owned configuration over deterministic defaults. */
bool Parser::client_settings(client::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    client::Settings candidate = output;
    bool hasUserInterface = false;
    bool hasExternalServer = false;
    bool hasCustomBootflowTextures = false;
    bool hasInvertSplashScreens = false;
    bool hasTitleFiligree = false;
    bool hasDumpGpuEntries = false;
    bool hasFadeRelease = false;
    bool hasForceJoinRequestReady = false;
    bool hasRegionPrivate = false;
    bool hasPinReplicatedRecord = false;
    bool hasRosterForceAuthored = false;
    bool hasHoldSpawn = false;
    bool hasSpawnHoldMs = false;
    bool hasSeedAuthoredSensors = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "ui") {
            if (hasUserInterface || !client_ui_settings(candidate.userInterface)) {
                return false;
            }
            hasUserInterface = true;
        } else if (key == "external_server") {
            if (hasExternalServer || !client_external_settings(candidate.externalServer)) {
                return false;
            }
            hasExternalServer = true;
        } else if (key == "custom_bootflow_textures") {
            if (hasCustomBootflowTextures || !boolean(candidate.customBootflowTextures)) {
                return false;
            }
            hasCustomBootflowTextures = true;
        } else if (key == "invert_splash_screens") {
            if (hasInvertSplashScreens || !boolean(candidate.invertSplashScreens)) {
                return false;
            }
            hasInvertSplashScreens = true;
        } else if (key == "title_filigree") {
            if (hasTitleFiligree || !boolean(candidate.titleFiligree)) {
                return false;
            }
            hasTitleFiligree = true;
        } else if (key == "dump_gpu_entries") {
            if (hasDumpGpuEntries || !boolean(candidate.dumpGpuEntries)) {
                return false;
            }
            hasDumpGpuEntries = true;
        } else if (key == "fade_release") {
            if (hasFadeRelease || !boolean(candidate.fadeRelease)) {
                return false;
            }
            hasFadeRelease = true;
        } else if (key == "force_join_request_ready") {
            if (hasForceJoinRequestReady || !boolean(candidate.forceJoinRequestReady)) {
                return false;
            }
            hasForceJoinRequestReady = true;
        } else if (key == "region_private") {
            if (hasRegionPrivate || !boolean(candidate.regionPrivate)) {
                return false;
            }
            hasRegionPrivate = true;
        } else if (key == "pin_replicated_record") {
            if (hasPinReplicatedRecord || !boolean(candidate.pinReplicatedRecord)) {
                return false;
            }
            hasPinReplicatedRecord = true;
        } else if (key == "roster_force_authored") {
            if (hasRosterForceAuthored || !boolean(candidate.rosterForceAuthored)) {
                return false;
            }
            hasRosterForceAuthored = true;
        } else if (key == "hold_spawn") {
            if (hasHoldSpawn || !boolean(candidate.holdSpawn)) {
                return false;
            }
            hasHoldSpawn = true;
        } else if (key == "spawn_hold_ms") {
            std::uint64_t value = 0;
            if (hasSpawnHoldMs || !unsigned_integer(value) || value == 0
                || value > client::kMaximumSpawnHoldMs) {
                return false;
            }
            candidate.spawnHoldMs = value;
            hasSpawnHoldMs = true;
        } else if (key == "seed_authored_sensors") {
            if (hasSeedAuthoredSensors || !boolean(candidate.seedAuthoredSensors)) {
                return false;
            }
            hasSeedAuthoredSensors = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace dawn::core::settings::parser
