#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem() = default;

    // Initialize with references to the M2 renderer (for model loading/instance spawning)
    void initialize(M2Renderer* m2Renderer);
    void shutdown();

    // Spawn a spell visual at a world position.
    // useImpactKit=false → CastKit path; useImpactKit=true → ImpactKit path
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

private:
    // Spell visual effects — transient M2 instances spawned by SMSG_PLAY_SPELL_VISUAL/IMPACT
    struct SpellVisualInstance {
        uint32_t instanceId;
        float elapsed;
        float duration;  // per-instance lifetime in seconds (from M2 anim or default)
    };

    void loadSpellVisualDbc();

    M2Renderer* m2Renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    std::unordered_map<uint32_t, std::string> spellVisualCastPath_;   // visualId → cast M2 path
    std::unordered_map<uint32_t, std::string> spellVisualImpactPath_; // visualId → impact M2 path
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    bool spellVisualDbcLoaded_ = false;
    static constexpr float SPELL_VISUAL_MAX_DURATION = 5.0f;
    static constexpr float SPELL_VISUAL_DEFAULT_DURATION = 2.0f;
};

} // namespace rendering
} // namespace wowee
