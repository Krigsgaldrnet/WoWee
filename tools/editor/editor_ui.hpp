#pragma once

#include "terrain_biomes.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace wowee {
namespace editor {

class EditorApp;

enum class PaintMode { Paint, Erase, ReplaceBase };

class EditorUI {
public:
    EditorUI();

    void render(EditorApp& app);
    void processActions(EditorApp& app);
    void openNewTerrainDialog() { showNewDialog_ = true; }
    void openLoadDialog() { showLoadDialog_ = true; }

    PaintMode getPaintMode() const { return paintMode_; }

    // Path point capture: when active, next terrain click sets the point
    enum class PathCapture { None, WaitingStart, WaitingEnd };
    PathCapture getPathCapture() const { return pathCapture_; }
    void setPathPoint(const glm::vec3& pos);
    glm::vec3 getPathStart() const { return pathStart_; }
    glm::vec3 getPathEnd() const { return pathEnd_; }
    bool isPathReady() const { return pathStartSet_ && pathEndSet_; }
    void clearPath() { pathStartSet_ = false; pathEndSet_ = false; pathCapture_ = PathCapture::None; }

private:
    void renderMenuBar(EditorApp& app);
    void renderToolbar(EditorApp& app);
    void renderNewTerrainDialog(EditorApp& app);
    void renderLoadDialog(EditorApp& app);
    void renderSaveDialog(EditorApp& app);
    void renderBrushPanel(EditorApp& app);
    void renderTexturePaintPanel(EditorApp& app);
    void renderObjectPanel(EditorApp& app);
    void renderWaterPanel(EditorApp& app);
    void renderNpcPanel(EditorApp& app);
    void renderQuestPanel(EditorApp& app);
    void renderContextMenu(EditorApp& app);
    void renderMinimap(EditorApp& app);
    void renderPropertiesPanel(EditorApp& app);
    void renderStatusBar(EditorApp& app);

    bool showNewDialog_ = false;
    bool showLoadDialog_ = false;
    bool showSaveDialog_ = false;
    bool showHelp_ = false;
    bool showAbout_ = false;
    bool generateAfterCreate_ = false;

    char newMapNameBuf_[256] = "CustomZone";
    int newTileX_ = 32;
    int newTileY_ = 32;
    float newBaseHeight_ = 100.0f;
    int newBiomeIdx_ = 0;
    bool newRequested_ = false;

    char loadMapNameBuf_[256] = "Azeroth";
    int loadTileX_ = 32;
    int loadTileY_ = 48;
    bool loadRequested_ = false;

    char savePathBuf_[512] = "";

    // Paint panel
    PaintMode paintMode_ = PaintMode::Paint;
    char texFilterBuf_[128] = "";
    int texDirIdx_ = -1; // -1 = all
    std::string selectedTexture_;

    // Object panel
    char objFilterBuf_[128] = "";
    int objDirIdx_ = -1;
    bool showM2s_ = true;
    bool showWMOs_ = true;

    // Path point capture
    PathCapture pathCapture_ = PathCapture::None;
    glm::vec3 pathStart_{0}, pathEnd_{0};
    bool pathStartSet_ = false, pathEndSet_ = false;
    int pathMode_ = 0; // 0=river, 1=road
    float pathWidth_ = 8.0f, pathDepth_ = 5.0f;
};

} // namespace editor
} // namespace wowee
