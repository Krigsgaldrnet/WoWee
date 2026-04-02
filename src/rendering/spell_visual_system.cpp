#include "rendering/spell_visual_system.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>

namespace wowee {
namespace rendering {

void SpellVisualSystem::initialize(M2Renderer* m2Renderer) {
    m2Renderer_ = m2Renderer;
}

void SpellVisualSystem::shutdown() {
    reset();
    m2Renderer_ = nullptr;
    cachedAssetManager_ = nullptr;
}

// Load SpellVisual DBC chain: SpellVisualEffectName → SpellVisualKit → SpellVisual
// to build cast/impact M2 path lookup maps.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;
    spellVisualDbcLoaded_ = true; // Set early to prevent re-entry on failure

    if (!cachedAssetManager_) {
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    }
    if (!cachedAssetManager_) return;

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;

    uint32_t svCastKitField   = svLayout  ? (*svLayout)["CastKit"]       : 2;
    uint32_t svImpactKitField = svLayout  ? (*svLayout)["ImpactKit"]     : 3;
    uint32_t svMissileField   = svLayout  ? (*svLayout)["MissileModel"]  : 8;
    uint32_t kitSpecial0Field = kitLayout ? (*kitLayout)["SpecialEffect0"] : 11;
    uint32_t kitBaseField     = kitLayout ? (*kitLayout)["BaseEffect"]     : 5;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;

    // Helper to look up effectName path from a kit ID
    // Load SpellVisualEffectName.dbc — ID → M2 path
    auto fxDbc = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!fxDbc || !fxDbc->isLoaded() || fxDbc->getFieldCount() <= fxFilePathField) {
        LOG_DEBUG("SpellVisual: SpellVisualEffectName.dbc unavailable (fc=",
                  fxDbc ? fxDbc->getFieldCount() : 0, ")");
        return;
    }
    std::unordered_map<uint32_t, std::string> effectPaths; // effectNameId → path
    for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
        uint32_t id   = fxDbc->getUInt32(i, 0);
        std::string p = fxDbc->getString(i, fxFilePathField);
        if (id && !p.empty()) effectPaths[id] = p;
    }

    // Load SpellVisualKit.dbc — kitId → best SpellVisualEffectName ID
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    std::unordered_map<uint32_t, uint32_t> kitToEffectName; // kitId → effectNameId
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            // Prefer SpecialEffect0, fall back to BaseEffect
            uint32_t eff = 0;
            if (kitSpecial0Field < fc) eff = kitDbc->getUInt32(i, kitSpecial0Field);
            if (!eff && kitBaseField < fc) eff = kitDbc->getUInt32(i, kitBaseField);
            if (eff) kitToEffectName[kitId] = eff;
        }
    }

    // Helper: resolve path for a given kit ID
    auto kitPath = [&](uint32_t kitId) -> std::string {
        if (!kitId) return {};
        auto kitIt = kitToEffectName.find(kitId);
        if (kitIt == kitToEffectName.end()) return {};
        auto fxIt = effectPaths.find(kitIt->second);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };
    auto missilePath = [&](uint32_t effId) -> std::string {
        if (!effId) return {};
        auto fxIt = effectPaths.find(effId);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };

    // Load SpellVisual.dbc — visualId → cast/impact M2 paths via kit chain
    auto svDbc = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    if (!svDbc || !svDbc->isLoaded()) {
        LOG_DEBUG("SpellVisual: SpellVisual.dbc unavailable");
        return;
    }
    uint32_t svFc = svDbc->getFieldCount();
    uint32_t loadedCast = 0, loadedImpact = 0;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        uint32_t vid = svDbc->getUInt32(i, 0);
        if (!vid) continue;

        // Cast path: CastKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        {
            std::string path;
            if (svCastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svCastKitField));
            if (path.empty() && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualCastPath_[vid] = path; ++loadedCast; }
        }
        // Impact path: ImpactKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        {
            std::string path;
            if (svImpactKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svImpactKitField));
            if (path.empty() && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualImpactPath_[vid] = path; ++loadedImpact; }
        }
    }
    LOG_INFO("SpellVisual: loaded cast=", loadedCast, " impact=", loadedImpact,
             " visual→M2 mappings (of ", svDbc->getRecordCount(), " records)");
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit) {
    if (!m2Renderer_ || visualId == 0) return;

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // Select cast or impact path map
    auto& pathMap = useImpactKit ? spellVisualImpactPath_ : spellVisualCastPath_;
    auto pathIt = pathMap.find(visualId);
    if (pathIt == pathMap.end()) return; // No model for this visual

    const std::string& modelPath = pathIt->second;

    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    // Skip models that have previously failed to load (avoid repeated I/O)
    if (spellVisualFailedModels_.count(modelId)) return;

    // Load the M2 model if not already loaded
    if (!m2Renderer_->hasModel(modelId)) {
        auto m2Data = cachedAssetManager_->readFile(modelPath);
        if (m2Data.empty()) {
            LOG_DEBUG("SpellVisual: could not read model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return;
        }
        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.vertices.empty() && model.particleEmitters.empty()) {
            LOG_DEBUG("SpellVisual: empty model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return;
        }
        // Load skin file for WotLK-format M2s
        if (model.version >= 264) {
            std::string skinPath = modelPath.substr(0, modelPath.rfind('.')) + "00.skin";
            auto skinData = cachedAssetManager_->readFile(skinPath);
            if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, model);
        }
        if (!m2Renderer_->loadModel(model, modelId)) {
            LOG_WARNING("SpellVisual: failed to load model to GPU: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return;
        }
        LOG_DEBUG("SpellVisual: loaded model id=", modelId, " path=", modelPath);
    }

    // Spawn instance at world position
    uint32_t instanceId = m2Renderer_->createInstance(modelId, worldPosition,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: failed to create instance for visualId=", visualId);
        return;
    }
    // Determine lifetime from M2 animation duration (clamp to reasonable range)
    float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    float duration = (animDurMs > 100.0f)
        ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
        : SPELL_VISUAL_DEFAULT_DURATION;
    activeSpellVisuals_.push_back({instanceId, 0.0f, duration});
    LOG_DEBUG("SpellVisual: spawned visualId=", visualId, " instanceId=", instanceId,
              " duration=", duration, "s model=", modelPath);
}

void SpellVisualSystem::update(float deltaTime) {
    if (activeSpellVisuals_.empty() || !m2Renderer_) return;
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        it->elapsed += deltaTime;
        if (it->elapsed >= it->duration) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::reset() {
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) {
        if (m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    }
    activeSpellVisuals_.clear();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

} // namespace rendering
} // namespace wowee
