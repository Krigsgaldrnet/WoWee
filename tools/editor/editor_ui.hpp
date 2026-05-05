#pragma once

#include "terrain_biomes.hpp"
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

    PaintMode getPaintMode() const { return paintMode_; }

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
};

} // namespace editor
} // namespace wowee
