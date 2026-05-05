#pragma once

#include "editor_camera.hpp"
#include "editor_viewport.hpp"
#include "editor_ui.hpp"
#include "terrain_editor.hpp"
#include "texture_painter.hpp"
#include "object_placer.hpp"
#include "npc_spawner.hpp"
#include "npc_presets.hpp"
#include "asset_browser.hpp"
#include "core/window.hpp"
#include "pipeline/asset_manager.hpp"
#include <string>
#include <memory>

namespace wowee {
namespace editor {

enum class EditorMode { Sculpt, Paint, PlaceObject, Water, NPC };

class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    bool initialize(const std::string& dataPath);
    void run();
    void shutdown();

    void loadADT(const std::string& mapName, int tileX, int tileY);
    void createNewTerrain(const std::string& mapName, int tileX, int tileY, float baseHeight, Biome biome);
    void saveADT(const std::string& path);
    void saveWDT(const std::string& path);

    void requestQuit();
    void resetCamera();
    void setWireframe(bool enabled);
    bool isWireframe() const;

    EditorCamera& getEditorCamera() { return camera_; }
    TerrainEditor& getTerrainEditor() { return terrainEditor_; }
    TexturePainter& getTexturePainter() { return texturePainter_; }
    ObjectPlacer& getObjectPlacer() { return objectPlacer_; }
    NpcSpawner& getNpcSpawner() { return npcSpawner_; }
    NpcPresets& getNpcPresets() { return npcPresets_; }
    AssetBrowser& getAssetBrowser() { return assetBrowser_; }
    rendering::TerrainRenderer* getTerrainRenderer();
    pipeline::AssetManager* getAssetManager() { return assetManager_.get(); }

    const std::string& getLoadedMap() const { return loadedMap_; }
    int getLoadedTileX() const { return loadedTileX_; }
    int getLoadedTileY() const { return loadedTileY_; }
    bool hasTerrainLoaded() const { return terrain_.isLoaded(); }

    core::Window* getWindow() { return window_.get(); }

    EditorMode getMode() const { return mode_; }
    void setMode(EditorMode m) { mode_ = m; }
    void markObjectsDirty() { objectsDirty_ = true; }

    void startGizmoMode(TransformMode mode);
    void setGizmoAxis(TransformAxis axis);
    TransformGizmo& getGizmo() { return viewport_.getGizmo(); }

    float getWaterHeight() const { return waterHeight_; }
    void setWaterHeight(float h) { waterHeight_ = h; }
    uint16_t getWaterType() const { return waterType_; }
    void setWaterType(uint16_t t) { waterType_ = t; }

private:
    void processEvents();
    void updateTerrainEditing(float dt);
    void refreshDirtyChunks();
    void initImGui();
    void shutdownImGui();

    std::unique_ptr<core::Window> window_;
    std::unique_ptr<pipeline::AssetManager> assetManager_;
    EditorCamera camera_;
    EditorViewport viewport_;
    EditorUI ui_;
    TerrainEditor terrainEditor_;
    TexturePainter texturePainter_;
    ObjectPlacer objectPlacer_;
    NpcSpawner npcSpawner_;
    NpcPresets npcPresets_;
    AssetBrowser assetBrowser_;

    pipeline::ADTTerrain terrain_;

    bool imguiInitialized_ = false;
    bool painting_ = false;
    bool objectsDirty_ = false;
    size_t lastObjectCount_ = 0;
    EditorMode mode_ = EditorMode::Sculpt;
    float waterHeight_ = 100.0f;
    uint16_t waterType_ = 0;
    std::string dataPath_;

    std::string loadedMap_;
    int loadedTileX_ = -1;
    int loadedTileY_ = -1;
};

} // namespace editor
} // namespace wowee
