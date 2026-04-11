#include "game/transport_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>
#include <limits>

namespace wowee::game {

TransportManager::TransportManager() = default;
TransportManager::~TransportManager() = default;

void TransportManager::update(float deltaTime) {
    elapsedTime_ += deltaTime;

    for (auto& [guid, transport] : transports_) {
        // Once we have server clock offset, we can predict server time indefinitely
        // No need for watchdog - keep using the offset even if server updates stop
        updateTransportMovement(transport, deltaTime);
    }
}

void TransportManager::registerTransport(uint64_t guid, uint32_t wmoInstanceId, uint32_t pathId, const glm::vec3& spawnWorldPos, uint32_t entry) {
    auto* pathEntry = pathRepo_.findPath(pathId);
    if (!pathEntry) {
        LOG_ERROR("TransportManager: Path ", pathId, " not found for transport ", guid);
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        LOG_ERROR("TransportManager: Path ", pathId, " has no waypoints");
        return;
    }

    ActiveTransport transport;
    transport.guid = guid;
    transport.wmoInstanceId = wmoInstanceId;
    transport.pathId = pathId;
    transport.entry = entry;
    transport.allowBootstrapVelocity = false;

    // CRITICAL: Set basePosition from spawn position and t=0 offset
    // For stationary paths (1 waypoint), just use spawn position directly
    if (spline.durationMs() == 0 || spline.keyCount() <= 1) {
        // Stationary transport - no path animation
        transport.basePosition = spawnWorldPos;
        transport.position = spawnWorldPos;
    } else if (pathEntry->worldCoords) {
        // World-coordinate path (TaxiPathNode) - points are absolute world positions
        transport.basePosition = glm::vec3(0.0f);
        transport.position = spline.evaluatePosition(0);
    } else {
        // Moving transport - infer base from first path offset
        glm::vec3 offset0 = spline.evaluatePosition(0);
        transport.basePosition = spawnWorldPos - offset0;  // Infer base from spawn
        transport.position = spawnWorldPos;  // Start at spawn position (base + offset0)

        // TransportAnimation paths are local offsets; first waypoint is expected near origin.
        // Warn only if the local path itself looks suspicious.
        glm::vec3 firstWaypoint = spline.keys()[0].position;
        if (glm::dot(firstWaypoint, firstWaypoint) > 100.0f) {
            LOG_WARNING("Transport 0x", std::hex, guid, std::dec, " path ", pathId,
                        ": first local waypoint far from origin: (",
                        firstWaypoint.x, ",", firstWaypoint.y, ",", firstWaypoint.z, ")");
        }
    }

    transport.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // Identity quaternion
    transport.playerOnBoard = false;
    transport.playerLocalOffset = glm::vec3(0.0f);
    transport.hasDeckBounds = false;
    transport.localClockMs = 0;
    transport.hasServerClock = false;
    transport.serverClockOffsetMs = 0;
    // Start with client-side animation for all DBC paths with real movement.
    // If the server sends actual position updates, updateServerTransport() will switch
    // to server-driven mode. This ensures transports like trams (which the server doesn't
    // stream updates for) still animate, while ships/zeppelins switch to server authority.
    transport.useClientAnimation = (pathEntry->fromDBC && spline.durationMs() > 0);
    transport.clientAnimationReverse = false;
    transport.serverYaw = 0.0f;
    transport.hasServerYaw = false;
    transport.serverYawFlipped180 = false;
    transport.serverYawAlignmentScore = 0;
    transport.lastServerUpdate = 0.0f;
    transport.serverUpdateCount = 0;
    transport.serverLinearVelocity = glm::vec3(0.0f);
    transport.serverAngularVelocity = 0.0f;
    transport.hasServerVelocity = false;

    if (transport.useClientAnimation && spline.durationMs() > 0) {
        // Seed to a stable phase based on our local clock so elevators don't all start at t=0.
        transport.localClockMs = static_cast<uint32_t>(elapsedTime_ * 1000.0) % spline.durationMs();
        LOG_INFO("TransportManager: Enabled client animation for transport 0x",
                 std::hex, guid, std::dec, " path=", pathId,
                 " durationMs=", spline.durationMs(), " seedMs=", transport.localClockMs,
                 (pathEntry->worldCoords ? " [worldCoords]" : (pathEntry->zOnly ? " [z-only]" : "")));
    }

    updateTransformMatrices(transport);

    // CRITICAL: Update WMO renderer with initial transform
    if (transport.isM2) {
        if (m2Renderer_) m2Renderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    } else {
        if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    }

    transports_[guid] = transport;

    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);
    LOG_INFO("TransportManager: Registered transport 0x", std::hex, guid, std::dec,
             " at path ", pathId, " with ", (pathEntry ? pathEntry->spline.keyCount() : 0u), " waypoints",
             " wmoInstanceId=", wmoInstanceId,
             " spawnPos=(", spawnWorldPos.x, ", ", spawnWorldPos.y, ", ", spawnWorldPos.z, ")",
             " basePos=(", transport.basePosition.x, ", ", transport.basePosition.y, ", ", transport.basePosition.z, ")",
             " initialRenderPos=(", renderPos.x, ", ", renderPos.y, ", ", renderPos.z, ")");
}

void TransportManager::unregisterTransport(uint64_t guid) {
    transports_.erase(guid);
    LOG_INFO("TransportManager: Unregistered transport ", guid);
}

ActiveTransport* TransportManager::getTransport(uint64_t guid) {
    auto it = transports_.find(guid);
    if (it != transports_.end()) {
        return &it->second;
    }
    return nullptr;
}

glm::vec3 TransportManager::getPlayerWorldPosition(uint64_t transportGuid, const glm::vec3& localOffset) {
    auto* transport = getTransport(transportGuid);
    if (!transport) {
        LOG_WARNING("getPlayerWorldPosition: transport 0x", std::hex, transportGuid, std::dec,
                    " not found — returning localOffset as-is (callers should guard)");
        return localOffset;
    }

    if (transport->isM2) {
        // M2 transports (trams): localOffset is a canonical world-space delta
        // from the transport's canonical position. Just add directly.
        return transport->position + localOffset;
    }

    // WMO transports (ships): localOffset is in transport-local space,
    // use the render-space transform matrix.
    glm::vec4 localPos(localOffset, 1.0f);
    glm::vec4 worldPos = transport->transform * localPos;
    return glm::vec3(worldPos);
}

glm::mat4 TransportManager::getTransportInvTransform(uint64_t transportGuid) {
    auto* transport = getTransport(transportGuid);
    if (!transport) {
        return glm::mat4(1.0f);  // Identity fallback
    }
    return transport->invTransform;
}

void TransportManager::loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints, bool looping, float speed) {
    pathRepo_.loadPathFromNodes(pathId, waypoints, looping, speed);
}

void TransportManager::setDeckBounds(uint64_t guid, const glm::vec3& min, const glm::vec3& max) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_ERROR("TransportManager: Cannot set deck bounds for unknown transport ", guid);
        return;
    }

    transport->deckMin = min;
    transport->deckMax = max;
    transport->hasDeckBounds = true;
}

void TransportManager::updateTransportMovement(ActiveTransport& transport, float deltaTime) {
    auto* pathEntry = pathRepo_.findPath(transport.pathId);
    if (!pathEntry) {
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        return;
    }

    // Stationary transport (durationMs = 0)
    if (spline.durationMs() == 0) {
        // Just update transform (position already set)
        updateTransformMatrices(transport);
        if (transport.isM2) {
            if (m2Renderer_) m2Renderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
        } else {
            if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
        }
        return;
    }

    // Evaluate path time
    uint32_t nowMs = static_cast<uint32_t>(elapsedTime_ * 1000.0);
    uint32_t pathTimeMs = 0;
    uint32_t durationMs = spline.durationMs();

    if (transport.hasServerClock) {
        // Predict server time using clock offset (works for both client and server-driven modes)
        int64_t serverTimeMs = static_cast<int64_t>(nowMs) + transport.serverClockOffsetMs;
        int64_t mod = static_cast<int64_t>(durationMs);
        int64_t wrapped = serverTimeMs % mod;
        if (wrapped < 0) wrapped += mod;
        pathTimeMs = static_cast<uint32_t>(wrapped);
    } else if (transport.useClientAnimation) {
        // Pure local clock (no server sync yet, client-driven)
        uint32_t dtMs = static_cast<uint32_t>(deltaTime * 1000.0f);
        if (!transport.clientAnimationReverse) {
            transport.localClockMs += dtMs;
        } else {
            if (dtMs > durationMs) {
                dtMs %= durationMs;
            }
            if (transport.localClockMs >= dtMs) {
                transport.localClockMs -= dtMs;
            } else {
                transport.localClockMs = durationMs - (dtMs - transport.localClockMs);
            }
        }
        pathTimeMs = transport.localClockMs % durationMs;
    } else {
        // Strict server-authoritative mode: do not guess movement between server snapshots.
        updateTransformMatrices(transport);
        if (transport.isM2) {
            if (m2Renderer_) m2Renderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
        } else {
            if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
        }
        return;
    }

    // Evaluate position from time via CatmullRomSpline (path is local offsets, add base position)
    glm::vec3 pathOffset = spline.evaluatePosition(pathTimeMs);
    // Guard against bad fallback Z curves on some remapped transport paths (notably icebreakers),
    // where path offsets can sink far below sea level when we only have spawn-time data.
    // Skip Z clamping for world-coordinate paths (TaxiPathNode) where values are absolute positions.
    // Clamp fallback Z offsets for non-world-coordinate paths to prevent transport
    // models from sinking below sea level on paths derived only from spawn-time data
    // (notably icebreaker routes where the DBC path has steep vertical curves).
    constexpr float kMinFallbackZOffset = -2.0f;
    constexpr float kMaxFallbackZOffset =  8.0f;
    if (!pathEntry->worldCoords) {
        if (transport.useClientAnimation && transport.serverUpdateCount <= 1) {
            pathOffset.z = glm::max(pathOffset.z, kMinFallbackZOffset);
        }
        if (!transport.useClientAnimation && !transport.hasServerClock) {
            pathOffset.z = glm::clamp(pathOffset.z, kMinFallbackZOffset, kMaxFallbackZOffset);
        }
    }
    transport.position = transport.basePosition + pathOffset;

    // Use server yaw if available (authoritative), otherwise compute from spline tangent
    if (transport.hasServerYaw) {
        float effectiveYaw = transport.serverYaw + (transport.serverYawFlipped180 ? glm::pi<float>() : 0.0f);
        transport.rotation = glm::angleAxis(effectiveYaw, glm::vec3(0.0f, 0.0f, 1.0f));
    } else {
        auto result = spline.evaluate(pathTimeMs);
        transport.rotation = orientationFromSplineTangent(result.tangent);
    }

    // Update transform matrices
    updateTransformMatrices(transport);

    // Update WMO instance position
    if (transport.isM2) {
        if (m2Renderer_) m2Renderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    } else {
        if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    }

    // Debug logging every 600 frames (~10 seconds at 60fps)
    static int debugFrameCount = 0;
    if (debugFrameCount++ % 600 == 0) {
        LOG_DEBUG("Transport 0x", std::hex, transport.guid, std::dec,
                 " pathTime=", pathTimeMs, "ms / ", durationMs, "ms",
                 " pos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")",
                 " mode=", (transport.useClientAnimation ? "client" : "server"),
                 " isM2=", transport.isM2);
    }
}

// Legacy transport orientation from spline tangent.
// Preserves the original TransportManager cross-product order for visual consistency.
glm::quat TransportManager::orientationFromSplineTangent(const glm::vec3& tangent) {
    float tangentLenSq = glm::dot(tangent, tangent);
    if (tangentLenSq < 1e-6f) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // Identity
    }

    glm::vec3 forward = tangent * glm::inversesqrt(tangentLenSq);
    glm::vec3 up(0.0f, 0.0f, 1.0f);  // WoW Z is up

    // If forward is nearly vertical, use different up vector
    if (std::abs(forward.z) > 0.99f) {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    glm::vec3 right = glm::normalize(glm::cross(up, forward));
    up = glm::cross(forward, right);

    // Build rotation matrix and convert to quaternion
    glm::mat3 rotMat;
    rotMat[0] = right;
    rotMat[1] = forward;
    rotMat[2] = up;

    return glm::quat_cast(rotMat);
}

void TransportManager::updateTransformMatrices(ActiveTransport& transport) {
    // Convert position from canonical to render coordinates for WMO rendering
    // Canonical: +X=North, +Y=West, +Z=Up
    // Render: renderX=wowY (west), renderY=wowX (north), renderZ=wowZ (up)
    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);

    // Convert rotation from canonical to render space using proper basis change
    // Canonical → Render is a 90° CCW rotation around Z (swaps X and Y)
    // Proper formula: q_render = q_basis * q_canonical * q_basis^-1
    glm::quat basisRotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::quat basisInverse = glm::conjugate(basisRotation);
    glm::quat renderRot = basisRotation * transport.rotation * basisInverse;

    // Build transform matrix: translate * rotate * scale
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), renderPos);
    glm::mat4 rotation = glm::mat4_cast(renderRot);
    glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));  // No scaling for transports

    transport.transform = translation * rotation * scale;
    transport.invTransform = glm::inverse(transport.transform);
}

void TransportManager::updateServerTransport(uint64_t guid, const glm::vec3& position, float orientation) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_WARNING("TransportManager::updateServerTransport: Transport not found: 0x", std::hex, guid, std::dec);
        return;
    }

    const bool hadPrevUpdate = (transport->serverUpdateCount > 0);
    const float prevUpdateTime = transport->lastServerUpdate;
    const glm::vec3 prevPos = transport->position;

    auto* pathEntry = pathRepo_.findPath(transport->pathId);
    const bool hasPath = (pathEntry != nullptr);
    const bool isZOnlyPath = (hasPath && pathEntry->fromDBC && pathEntry->zOnly && pathEntry->spline.durationMs() > 0);
    const bool isWorldCoordPath = (hasPath && pathEntry->worldCoords && pathEntry->spline.durationMs() > 0);

    // Don't let (0,0,0) server updates override a TaxiPathNode world-coordinate path
    if (isWorldCoordPath && glm::dot(position, position) < 1.0f) {
        transport->serverUpdateCount++;
        transport->lastServerUpdate = elapsedTime_;
        transport->serverYaw = orientation;
        transport->hasServerYaw = true;
        return;
    }

    // Track server updates
    transport->serverUpdateCount++;
    transport->lastServerUpdate = elapsedTime_;
    // Z-only elevators and world-coordinate paths (TaxiPathNode) always stay client-driven.
    // For other DBC paths (trams, ships): only switch to server-driven mode when the server
    // sends a position that actually differs from the current position, indicating it's
    // actively streaming movement data (not just echoing the spawn position).
    if (isZOnlyPath || isWorldCoordPath) {
        transport->useClientAnimation = true;
    } else if (transport->useClientAnimation && hasPath && pathEntry->fromDBC) {
        glm::vec3 pd = position - transport->position;
        float posDeltaSq = glm::dot(pd, pd);
        if (posDeltaSq > 1.0f) {
            // Server sent a meaningfully different position — it's actively driving this transport
            transport->useClientAnimation = false;
            LOG_INFO("Transport 0x", std::hex, guid, std::dec,
                     " switching to server-driven (posDeltaSq=", posDeltaSq, ")");
        }
        // Otherwise keep client animation (server just echoed spawn pos or sent small jitter)
    } else if (!hasPath || !pathEntry->fromDBC) {
        // No DBC path — purely server-driven
        transport->useClientAnimation = false;
    }
    transport->clientAnimationReverse = false;

    if (!hasPath || pathEntry->spline.durationMs() == 0) {
        // No path or stationary - just set position directly
        transport->basePosition = position;
        transport->position = position;
        transport->rotation = glm::angleAxis(orientation, glm::vec3(0.0f, 0.0f, 1.0f));
        updateTransformMatrices(*transport);
        if (transport->isM2) {
            if (m2Renderer_) m2Renderer_->setInstanceTransform(transport->wmoInstanceId, transport->transform);
        } else {
            if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport->wmoInstanceId, transport->transform);
        }
        return;
    }

    // Server-authoritative transport mode:
    // Trust explicit server world position/orientation directly for all moving transports.
    // This avoids wrong-route and direction errors when local DBC path mapping differs from server route IDs.
    transport->hasServerClock = false;
    if (transport->serverUpdateCount == 1) {
        // Seed once from first authoritative update; keep stable base for fallback phase estimation.
        // For z-only elevator paths, keep the spawn-derived basePosition (the DBC path is local offsets).
        if (!isZOnlyPath) {
            transport->basePosition = position;
        }
    }
    transport->position = position;
    transport->serverYaw = orientation;
    transport->hasServerYaw = true;
    float effectiveYaw = transport->serverYaw + (transport->serverYawFlipped180 ? glm::pi<float>() : 0.0f);
    transport->rotation = glm::angleAxis(effectiveYaw, glm::vec3(0.0f, 0.0f, 1.0f));

    if (hadPrevUpdate) {
        const float dt = elapsedTime_ - prevUpdateTime;
        if (dt > 0.001f) {
            glm::vec3 v = (position - prevPos) / dt;
            float speedSq = glm::dot(v, v);
            constexpr float kMinAuthoritativeSpeed = 0.15f;
            constexpr float kMaxSpeed = 60.0f;
            if (speedSq >= kMinAuthoritativeSpeed * kMinAuthoritativeSpeed) {
                // Auto-detect 180-degree yaw mismatch by comparing heading to movement direction.
                // Some transports appear to report yaw opposite their actual travel direction.
                glm::vec2 horizontalV(v.x, v.y);
                float hLenSq = glm::dot(horizontalV, horizontalV);
                if (hLenSq > 0.04f) {
                    horizontalV *= glm::inversesqrt(hLenSq);
                    glm::vec2 heading(std::cos(transport->serverYaw), std::sin(transport->serverYaw));
                    float alignDot = glm::dot(heading, horizontalV);

                    if (alignDot < -0.35f) {
                        transport->serverYawAlignmentScore = std::max(transport->serverYawAlignmentScore - 1, -12);
                    } else if (alignDot > 0.35f) {
                        transport->serverYawAlignmentScore = std::min(transport->serverYawAlignmentScore + 1, 12);
                    }

                    if (!transport->serverYawFlipped180 && transport->serverYawAlignmentScore <= -4) {
                        transport->serverYawFlipped180 = true;
                        LOG_INFO("Transport 0x", std::hex, guid, std::dec,
                                 " enabled 180-degree yaw correction (alignScore=",
                                 transport->serverYawAlignmentScore, ")");
                    } else if (transport->serverYawFlipped180 &&
                               transport->serverYawAlignmentScore >= 4) {
                        transport->serverYawFlipped180 = false;
                        LOG_INFO("Transport 0x", std::hex, guid, std::dec,
                                 " disabled 180-degree yaw correction (alignScore=",
                                 transport->serverYawAlignmentScore, ")");
                    }
                }

                if (speedSq > kMaxSpeed * kMaxSpeed) {
                    v *= (kMaxSpeed * glm::inversesqrt(speedSq));
                }

                transport->serverLinearVelocity = v;
                transport->serverAngularVelocity = 0.0f;
                transport->hasServerVelocity = true;

                // Re-apply potentially corrected yaw this frame after alignment check.
                effectiveYaw = transport->serverYaw + (transport->serverYawFlipped180 ? glm::pi<float>() : 0.0f);
                transport->rotation = glm::angleAxis(effectiveYaw, glm::vec3(0.0f, 0.0f, 1.0f));
            }
        }
    } else {
        // Seed fallback path phase from nearest waypoint to the first authoritative sample.
        if (pathEntry && pathEntry->spline.keyCount() > 0 && pathEntry->spline.durationMs() > 0) {
            glm::vec3 local = position - transport->basePosition;
            size_t bestIdx = pathEntry->spline.findNearestKey(local);
            transport->localClockMs = pathEntry->spline.keys()[bestIdx].timeMs % pathEntry->spline.durationMs();
        }

        // Bootstrap velocity from mapped DBC path on first authoritative sample.
        // This avoids "stalled at dock" when server sends sparse transport snapshots.
        if (transport->allowBootstrapVelocity && pathEntry && pathEntry->spline.keyCount() >= 2 && pathEntry->spline.durationMs() > 0) {
            const auto& keys = pathEntry->spline.keys();
            glm::vec3 local = position - transport->basePosition;
            size_t bestIdx = pathEntry->spline.findNearestKey(local);

            float bestDistSq = 0.0f;
            {
                glm::vec3 d = keys[bestIdx].position - local;
                bestDistSq = glm::dot(d, d);
            }

                constexpr float kMaxBootstrapNearestDist = 80.0f;
                if (bestDistSq > (kMaxBootstrapNearestDist * kMaxBootstrapNearestDist)) {
                    LOG_WARNING("Transport 0x", std::hex, guid, std::dec,
                                " skipping DBC bootstrap velocity: nearest path point too far (dist=",
                                std::sqrt(bestDistSq), ", path=", transport->pathId, ")");
                } else {
                    size_t n = keys.size();
                    uint32_t durMs = pathEntry->spline.durationMs();
                    constexpr float kMinBootstrapSpeed = 0.25f;
                    constexpr float kMaxSpeed = 60.0f;

                    auto tryApplySegment = [&](size_t a, size_t b) {
                        uint32_t t0 = keys[a].timeMs;
                        uint32_t t1 = keys[b].timeMs;
                        if (b == 0 && t1 <= t0 && durMs > 0) {
                            t1 = durMs;
                        }
                        if (t1 <= t0) return;
                        glm::vec3 seg = keys[b].position - keys[a].position;
                        float dtSeg = static_cast<float>(t1 - t0) / 1000.0f;
                        if (dtSeg <= 0.001f) return;
                        glm::vec3 v = seg / dtSeg;
                        float speedSq = glm::dot(v, v);
                        if (speedSq < kMinBootstrapSpeed * kMinBootstrapSpeed) return;
                        if (speedSq > kMaxSpeed * kMaxSpeed) {
                            v *= (kMaxSpeed * glm::inversesqrt(speedSq));
                        }
                        transport->serverLinearVelocity = v;
                        transport->serverAngularVelocity = 0.0f;
                        transport->hasServerVelocity = true;
                    };

                    // Prefer nearest forward meaningful segment from bestIdx.
                    for (size_t step = 1; step < n && !transport->hasServerVelocity; ++step) {
                        size_t a = (bestIdx + step - 1) % n;
                        size_t b = (bestIdx + step) % n;
                        tryApplySegment(a, b);
                    }
                    // Fallback: nearest backward meaningful segment.
                    for (size_t step = 1; step < n && !transport->hasServerVelocity; ++step) {
                        size_t b = (bestIdx + n - step + 1) % n;
                        size_t a = (bestIdx + n - step) % n;
                        tryApplySegment(a, b);
                    }

                    if (transport->hasServerVelocity) {
                        LOG_INFO("Transport 0x", std::hex, guid, std::dec,
                                 " bootstrapped velocity from DBC path ", transport->pathId,
                                 " v=(", transport->serverLinearVelocity.x, ", ",
                                 transport->serverLinearVelocity.y, ", ",
                                 transport->serverLinearVelocity.z, ")");
                    } else {
                        LOG_INFO("Transport 0x", std::hex, guid, std::dec,
                                 " skipped DBC bootstrap velocity (segment too short/static)");
                    }
                }
        } else if (!transport->allowBootstrapVelocity) {
            LOG_INFO("Transport 0x", std::hex, guid, std::dec,
                     " DBC bootstrap velocity disabled for this transport");
        }
    }

    updateTransformMatrices(*transport);
    if (transport->isM2) {
        if (m2Renderer_) m2Renderer_->setInstanceTransform(transport->wmoInstanceId, transport->transform);
    } else {
        if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport->wmoInstanceId, transport->transform);
    }
    return;
}

bool TransportManager::loadTransportAnimationDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTransportAnimationDBC(assetMgr);
}

bool TransportManager::loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTaxiPathNodeDBC(assetMgr);
}

bool TransportManager::hasTaxiPath(uint32_t taxiPathId) const {
    return pathRepo_.hasTaxiPath(taxiPathId);
}

bool TransportManager::assignTaxiPathToTransport(uint32_t entry, uint32_t taxiPathId) {
    auto* taxiEntry = pathRepo_.findTaxiPath(taxiPathId);
    if (!taxiEntry) {
        LOG_WARNING("No TaxiPathNode path for taxiPathId=", taxiPathId);
        return false;
    }

    // Find transport(s) with matching entry that are at (0,0,0)
    for (auto& [guid, transport] : transports_) {
        if (transport.entry != entry) continue;
        if (glm::dot(transport.position, transport.position) > 1.0f) continue;  // Already has real position

        // Copy the taxi path into the main paths (indexed by GO entry for this transport)
        PathEntry copied(taxiEntry->spline, entry, taxiEntry->zOnly, taxiEntry->fromDBC, taxiEntry->worldCoords);
        pathRepo_.storePath(entry, std::move(copied));

        auto* storedEntry = pathRepo_.findPath(entry);

        // Update transport to use the new path
        transport.pathId = entry;
        transport.basePosition = glm::vec3(0.0f);  // World-coordinate path, no base offset
        if (storedEntry && storedEntry->spline.keyCount() > 0) {
            transport.position = storedEntry->spline.evaluatePosition(0);
        }
        transport.useClientAnimation = true;  // Server won't send position updates

        // Seed local clock to a deterministic phase
        if (storedEntry && storedEntry->spline.durationMs() > 0) {
            transport.localClockMs = static_cast<uint32_t>(elapsedTime_ * 1000.0) % storedEntry->spline.durationMs();
        }

        updateTransformMatrices(transport);
        if (wmoRenderer_) {
            wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
        }

        LOG_INFO("Assigned TaxiPathNode path to transport 0x", std::hex, guid, std::dec,
                 " entry=", entry, " taxiPathId=", taxiPathId,
                 " waypoints=", storedEntry ? storedEntry->spline.keyCount() : 0u,
                 " duration=", storedEntry ? storedEntry->spline.durationMs() : 0u, "ms",
                 " startPos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")");
        return true;
    }

    LOG_DEBUG("No transport at (0,0,0) found for entry=", entry, " taxiPathId=", taxiPathId);
    return false;
}

bool TransportManager::hasPathForEntry(uint32_t entry) const {
    return pathRepo_.hasPathForEntry(entry);
}

bool TransportManager::hasUsableMovingPathForEntry(uint32_t entry, float minXYRange) const {
    return pathRepo_.hasUsableMovingPathForEntry(entry, minXYRange);
}

uint32_t TransportManager::inferDbcPathForSpawn(const glm::vec3& spawnWorldPos,
                                               float maxDistance,
                                               bool allowZOnly) const {
    return pathRepo_.inferDbcPathForSpawn(spawnWorldPos, maxDistance, allowZOnly);
}

uint32_t TransportManager::inferMovingPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance) const {
    return pathRepo_.inferMovingPathForSpawn(spawnWorldPos, maxDistance);
}

uint32_t TransportManager::pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const {
    return pathRepo_.pickFallbackMovingPath(entry, displayId);
}

} // namespace wowee::game
