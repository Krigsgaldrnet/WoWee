#include "editor_ui.hpp"
#include "editor_app.hpp"
#include "terrain_editor.hpp"
#include "texture_painter.hpp"
#include "object_placer.hpp"
#include "npc_spawner.hpp"
#include "npc_presets.hpp"
#include "asset_browser.hpp"
#include "transform_gizmo.hpp"
#include "terrain_biomes.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/camera.hpp"
#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cctype>

namespace wowee {
namespace editor {

EditorUI::EditorUI() = default;

static bool matchesFilter(const std::string& text, const std::string& filter) {
    if (filter.empty()) return true;
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower.find(filter) != std::string::npos;
}

void EditorUI::render(EditorApp& app) {
    renderMenuBar(app);
    renderToolbar(app);
    if (showNewDialog_) renderNewTerrainDialog(app);
    if (showLoadDialog_) renderLoadDialog(app);
    if (showSaveDialog_) renderSaveDialog(app);

    switch (app.getMode()) {
        case EditorMode::Sculpt: renderBrushPanel(app); break;
        case EditorMode::Paint: renderTexturePaintPanel(app); break;
        case EditorMode::PlaceObject: renderObjectPanel(app); break;
        case EditorMode::Water: renderWaterPanel(app); break;
        case EditorMode::NPC: renderNpcPanel(app); break;
    }

    renderContextMenu(app);
    renderMinimap(app);
    renderPropertiesPanel(app);
    renderStatusBar(app);

    // Toast notifications
    ImGuiViewport* tvp = ImGui::GetMainViewport();
    float toastY = tvp->Size.y - 60;
    for (const auto& t : app.getToasts()) {
        float alpha = std::min(1.0f, t.timer);
        ImGui::SetNextWindowPos(ImVec2(tvp->Size.x / 2 - 150, toastY));
        ImGui::SetNextWindowSize(ImVec2(300, 30));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.4f, 0.1f, 0.9f));
        char toastId[32]; std::snprintf(toastId, sizeof(toastId), "##toast%p", (void*)&t);
        ImGui::Begin(toastId, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("%s", t.msg.c_str());
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        toastY -= 35;
    }
}

void EditorUI::processActions(EditorApp& app) {
    if (newRequested_) {
        newRequested_ = false;
        app.createNewTerrain(newMapNameBuf_, newTileX_, newTileY_, newBaseHeight_,
                             static_cast<Biome>(newBiomeIdx_));
    }
    if (loadRequested_) {
        loadRequested_ = false;
        app.loadADT(loadMapNameBuf_, loadTileX_, loadTileY_);
    }
}

void EditorUI::renderMenuBar(EditorApp& app) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Terrain...", "Ctrl+N")) showNewDialog_ = true;
            if (ImGui::MenuItem("Load ADT...", "Ctrl+O")) showLoadDialog_ = true;
            if (ImGui::BeginMenu("Import Heightmap", app.hasTerrainLoaded())) {
                static char hmPath[256] = "heightmap.raw";
                static float hmScale = 200.0f;
                ImGui::InputText("File##hm", hmPath, sizeof(hmPath));
                ImGui::SliderFloat("Height Scale", &hmScale, 10.0f, 1000.0f);
                ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1), "RAW 16-bit or 8-bit (129x129 or 257x257)");
                if (ImGui::MenuItem("Import")) {
                    if (app.getTerrainEditor().importHeightmap(hmPath, hmScale))
                        app.showToast("Heightmap imported");
                    else
                        app.showToast("Failed to import heightmap");
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Clear All Objects/NPCs", nullptr, false, app.hasTerrainLoaded())) {
                app.getObjectPlacer().clearAll();
                app.getNpcSpawner().clearSelection();
                app.getNpcSpawner().getSpawns().clear();
                app.getTerrainEditor().history().clear();
                app.markObjectsDirty();
                app.showToast("All objects and NPCs cleared");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quick Save", "Ctrl+S", false, app.hasTerrainLoaded()))
                app.quickSave();
            if (ImGui::MenuItem("Export Zone...", nullptr, false, app.hasTerrainLoaded()))
                showSaveDialog_ = true;
            if (ImGui::BeginMenu("Add Adjacent Tile", app.hasTerrainLoaded())) {
                if (ImGui::MenuItem("North (+X)")) app.addAdjacentTile(1, 0);
                if (ImGui::MenuItem("South (-X)")) app.addAdjacentTile(-1, 0);
                if (ImGui::MenuItem("East (+Y)")) app.addAdjacentTile(0, 1);
                if (ImGui::MenuItem("West (-Y)")) app.addAdjacentTile(0, -1);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Export Heightmap", app.hasTerrainLoaded())) {
                static char expHmPath[256] = "output/heightmap.raw";
                static float expHmScale = 500.0f;
                ImGui::InputText("File##exphm", expHmPath, sizeof(expHmPath));
                ImGui::SliderFloat("Max Height##exphm", &expHmScale, 50.0f, 2000.0f);
                if (ImGui::MenuItem("Export 16-bit RAW (129x129)")) {
                    if (app.getTerrainEditor().exportHeightmap(expHmPath, expHmScale))
                        app.showToast("Heightmap exported");
                    else
                        app.showToast("Export failed");
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) app.requestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            auto& te = app.getTerrainEditor();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, te.history().canUndo())) te.undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, te.history().canRedo())) te.redo();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            bool wf = app.isWireframe();
            if (ImGui::MenuItem("Wireframe", "F3", &wf)) app.setWireframe(wf);
            bool fog = false;
            auto* tr = app.getTerrainRenderer();
            if (tr) fog = tr->isFogEnabled();
            if (ImGui::MenuItem("Fog", nullptr, &fog) && tr) tr->setFogEnabled(fog);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Camera")) app.resetCamera();
            if (ImGui::MenuItem("Center on Terrain", "Home")) app.centerOnTerrain();
            ImGui::Separator();
            if (ImGui::BeginMenu("Sky / Lighting")) {
                if (ImGui::MenuItem("Day")) app.setSkyPreset(0);
                if (ImGui::MenuItem("Dusk")) app.setSkyPreset(1);
                if (ImGui::MenuItem("Night")) app.setSkyPreset(2);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Bookmark", "F5")) app.saveBookmark("");
            auto& bmarks = app.getBookmarks();
            if (!bmarks.empty() && ImGui::BeginMenu("Load Bookmark")) {
                for (int i = 0; i < static_cast<int>(bmarks.size()); i++) {
                    if (ImGui::MenuItem(bmarks[i].name.c_str()))
                        app.loadBookmark(i);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Keyboard Shortcuts", "F1")) showHelp_ = !showHelp_;
            ImGui::Separator();
            if (ImGui::MenuItem("About Wowee Editor")) {
                ImGui::OpenPopup("AboutEditor");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginPopup("AboutEditor")) {
            ImGui::Text("Wowee World Editor");
            ImGui::Text("by Kelsi Davis");
            ImGui::Separator();
            ImGui::Text("Version: 0.1.0 (rough/WIP)");
            ImGui::Text("Standalone world editor for creating");
            ImGui::Text("custom WoW zones for the wowee client.");
            ImGui::Separator();
            ImGui::Text("Tools: Sculpt, Paint, Objects, Water, NPCs");
            ImGui::Text("Export: ADT + WDT + JSON (zone manifest)");
            ImGui::Text("Formats: WoW 3.3.5a compatible");
            ImGui::EndPopup();
        }
        ImGui::EndMainMenuBar();
    }

    // Help overlay
    if (showHelp_) {
        ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Keyboard Shortcuts", &showHelp_)) {
            ImGui::Text("Navigation:");
            ImGui::BulletText("WASD — fly camera");
            ImGui::BulletText("Q/E — descend/ascend");
            ImGui::BulletText("Right-drag — look around");
            ImGui::BulletText("Scroll — adjust camera speed");
            ImGui::BulletText("Shift — sprint");
            ImGui::Separator();
            ImGui::Text("Editing:");
            ImGui::BulletText("Left-click — paint/place (depends on mode)");
            ImGui::BulletText("Ctrl+click — select object/NPC");
            ImGui::BulletText("Ctrl+S — quick save");
            ImGui::BulletText("Ctrl+Z — undo");
            ImGui::BulletText("Ctrl+Shift+Z — redo");
            ImGui::BulletText("Delete — remove selected");
            ImGui::Separator();
            ImGui::Text("Object Transform:");
            ImGui::BulletText("G — move mode (then drag)");
            ImGui::BulletText("R — rotate mode (then drag)");
            ImGui::BulletText("T — scale mode (then drag)");
            ImGui::BulletText("X/Y — constrain to axis");
            ImGui::BulletText("Escape — deselect / cancel");
            ImGui::BulletText("Right-click — context menu");
            ImGui::Separator();
            ImGui::Text("View:");
            ImGui::Text("Modes:");
            ImGui::BulletText("1 — Sculpt");
            ImGui::BulletText("2 — Paint");
            ImGui::BulletText("3 — Objects");
            ImGui::BulletText("4 — Water");
            ImGui::BulletText("5 — NPCs");
            ImGui::Separator();
            ImGui::Text("View:");
            ImGui::BulletText("F1 — this help");
            ImGui::BulletText("F3 — wireframe toggle");
            ImGui::BulletText("F5 — save camera bookmark");
            ImGui::BulletText("Home — center on terrain");
            ImGui::BulletText("Scroll — zoom in/out");
            ImGui::BulletText("Shift+Scroll — adjust speed");
        }
        ImGui::End();
    }
}

void EditorUI::renderToolbar(EditorApp& app) {
    ImGui::SetNextWindowPos(ImVec2(300, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 50), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::Begin("##Toolbar", nullptr, flags)) {
        auto mode = app.getMode();

        auto modeButton = [&](const char* label, EditorMode m) {
            bool active = (mode == m);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            }
            if (ImGui::Button(label, ImVec2(70, 28)))
                app.setMode(m);
            if (active)
                ImGui::PopStyleColor(2);
            ImGui::SameLine();
        };

        modeButton("Sculpt", EditorMode::Sculpt);
        modeButton("Paint", EditorMode::Paint);
        modeButton("Objects", EditorMode::PlaceObject);
        modeButton("Water", EditorMode::Water);
        modeButton("NPCs", EditorMode::NPC);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorUI::renderNewTerrainDialog(EditorApp& /*app*/) {
    ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("New Terrain", &showNewDialog_)) {
        ImGui::InputText("Map Name", newMapNameBuf_, sizeof(newMapNameBuf_));
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
            "Internal name (no spaces). Used for file paths.");
        ImGui::InputInt("Tile X", &newTileX_);
        ImGui::InputInt("Tile Y", &newTileY_);
        ImGui::SliderFloat("Base Height", &newBaseHeight_, 0.0f, 500.0f);
        newTileX_ = std::max(0, std::min(63, newTileX_));
        newTileY_ = std::max(0, std::min(63, newTileY_));

        ImGui::Separator();
        const char* biomeNames[] = {
            "Grassland", "Forest", "Jungle", "Desert", "Barrens",
            "Snow", "Swamp", "Rocky", "Beach", "Volcanic"
        };
        ImGui::Combo("Biome", &newBiomeIdx_, biomeNames, 10);
        const auto& bt = getBiomeTextures(static_cast<Biome>(newBiomeIdx_));
        ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.5f, 1.0f), "%s", bt.base);

        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(120, 0))) { newRequested_ = true; showNewDialog_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) showNewDialog_ = false;
    }
    ImGui::End();
}

void EditorUI::renderLoadDialog(EditorApp& app) {
    ImGui::SetNextWindowSize(ImVec2(400, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Load Map Tile", &showLoadDialog_)) {
        // Map browser
        auto& maps = app.getAssetBrowser().getMapNames();
        static char mapFilter[64] = "";
        ImGui::InputText("Search Maps", mapFilter, sizeof(mapFilter));
        std::string filter(mapFilter);
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        ImGui::BeginChild("MapList", ImVec2(0, 180), true);
        for (const auto& m : maps) {
            if (!filter.empty() && m.find(filter) == std::string::npos) continue;
            bool selected = (m == std::string(loadMapNameBuf_));
            if (ImGui::Selectable(m.c_str(), selected))
                std::strncpy(loadMapNameBuf_, m.c_str(), sizeof(loadMapNameBuf_) - 1);
        }
        ImGui::EndChild();

        ImGui::Text("Selected: %s", loadMapNameBuf_);
        ImGui::InputInt("Tile X", &loadTileX_);
        ImGui::InputInt("Tile Y", &loadTileY_);
        loadTileX_ = std::max(0, std::min(63, loadTileX_));
        loadTileY_ = std::max(0, std::min(63, loadTileY_));

        // Check if the selected tile exists
        {
            std::string testPath = std::string("world\\maps\\") + loadMapNameBuf_ + "\\" +
                                   loadMapNameBuf_ + "_" + std::to_string(loadTileX_) + "_" +
                                   std::to_string(loadTileY_) + ".adt";
            std::string lower = testPath;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            bool exists = app.getAssetManager()->getManifest().hasEntry(lower);
            if (exists)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1), "Tile found in manifest");
            else
                ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "Tile not found — try different coords");
        }

        ImGui::Spacing();
        if (ImGui::Button("Load", ImVec2(120, 0))) { loadRequested_ = true; showLoadDialog_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) showLoadDialog_ = false;
    }
    ImGui::End();
}

void EditorUI::renderSaveDialog(EditorApp& app) {
    ImGui::SetNextWindowSize(ImVec2(500, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Export Zone", &showSaveDialog_)) {
        if (savePathBuf_[0] == '\0' && app.hasTerrainLoaded())
            std::snprintf(savePathBuf_, sizeof(savePathBuf_), "output");
        ImGui::InputText("Output Directory", savePathBuf_, sizeof(savePathBuf_));
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
            "Exports: ADT, WDT, and creature spawns to %s/%s/",
            savePathBuf_, app.getLoadedMap().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Export All", ImVec2(140, 0))) {
            app.exportZone(savePathBuf_);
            showSaveDialog_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) showSaveDialog_ = false;
    }
    ImGui::End();
}

void EditorUI::renderBrushPanel(EditorApp& app) {
    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sculpt Brush")) {
        if (!app.hasTerrainLoaded()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Load or create terrain first");
            ImGui::End(); return;
        }
        auto& s = app.getTerrainEditor().brush().settings();
        const char* modes[] = {"Raise", "Lower", "Smooth", "Flatten", "Level", "Erode"};
        int idx = static_cast<int>(s.mode);
        if (ImGui::Combo("Mode", &idx, modes, 6)) s.mode = static_cast<BrushMode>(idx);
        if (ImGui::IsItemHovered()) {
            const char* tips[] = {
                "Raise: lift terrain up",
                "Lower: push terrain down",
                "Smooth: average neighbor heights",
                "Flatten: set to target height",
                "Level: same as flatten",
                "Erode: simulate water erosion downhill"
            };
            ImGui::SetTooltip("%s", tips[idx]);
        }
        ImGui::SliderFloat("Radius", &s.radius, 5.0f, 200.0f, "%.0f");
        ImGui::SliderFloat("Strength", &s.strength, 0.5f, 50.0f, "%.1f");
        ImGui::SliderFloat("Falloff", &s.falloff, 0.0f, 1.0f, "%.2f");
        if (s.mode == BrushMode::Flatten || s.mode == BrushMode::Level) {
            ImGui::SliderFloat("Target Height", &s.flattenHeight, -500.0f, 1000.0f, "%.1f");
            ImGui::SameLine();
            auto& brush = app.getTerrainEditor().brush();
            if (ImGui::SmallButton("Pick") && brush.isActive())
                s.flattenHeight = brush.getPosition().z;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Set target height from cursor position");
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Noise Generator")) {
            static float noiseFreq = 0.005f;
            static float noiseAmp = 20.0f;
            static int noiseOctaves = 4;
            static int noiseSeed = 42;
            ImGui::SliderFloat("Frequency", &noiseFreq, 0.001f, 0.05f, "%.4f");
            ImGui::SliderFloat("Amplitude", &noiseAmp, 1.0f, 200.0f, "%.0f");
            ImGui::SliderInt("Octaves", &noiseOctaves, 1, 8);
            ImGui::InputInt("Seed", &noiseSeed);
            if (ImGui::Button("Apply Noise", ImVec2(-1, 0))) {
                app.getTerrainEditor().applyNoise(noiseFreq, noiseAmp, noiseOctaves,
                                                   static_cast<uint32_t>(noiseSeed));
                app.showToast("Noise applied");
            }
            static int smoothPasses = 2;
            ImGui::SliderInt("Smooth Passes", &smoothPasses, 1, 10);
            if (ImGui::Button("Smooth Entire Tile", ImVec2(-1, 0))) {
                app.getTerrainEditor().smoothEntireTile(smoothPasses);
                app.showToast("Tile smoothed");
            }
            ImGui::Separator();
            static float clampMin = 0.0f, clampMax = 500.0f;
            ImGui::DragFloatRange2("Clamp Range", &clampMin, &clampMax, 1.0f, -500.0f, 2000.0f);
            if (ImGui::Button("Clamp Heights", ImVec2(-1, 0))) {
                app.getTerrainEditor().clampHeights(clampMin, clampMax);
                app.showToast("Heights clamped");
            }
            ImGui::Separator();
            static float hScale = 1.5f;
            ImGui::SliderFloat("Height Scale", &hScale, 0.1f, 5.0f, "%.2f");
            if (ImGui::Button("Scale Heights", ImVec2(-1, 0))) {
                app.getTerrainEditor().scaleHeights(hScale);
                app.showToast("Heights scaled");
            }
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
                "Exaggerate (>1) or flatten (<1) terrain relief");
        }

        ImGui::Separator();
        ImGui::Text("Terrain Holes (cave entrances):");
        auto& brush = app.getTerrainEditor().brush();
        if (ImGui::Button("Punch Hole", ImVec2(120, 0)) && brush.isActive())
            app.getTerrainEditor().punchHole(brush.getPosition(), s.radius);
        ImGui::SameLine();
        if (ImGui::Button("Fill Hole", ImVec2(120, 0)) && brush.isActive())
            app.getTerrainEditor().fillHole(brush.getPosition(), s.radius);

        ImGui::Separator();
        auto& hist = app.getTerrainEditor().history();
        ImGui::Text("Undo: %zu  Redo: %zu", hist.undoCount(), hist.redoCount());
    }
    ImGui::End();
}

void EditorUI::renderTexturePaintPanel(EditorApp& app) {
    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 550), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Texture Paint")) {
        if (!app.hasTerrainLoaded()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Load or create terrain first");
            ImGui::End(); return;
        }

        auto& s = app.getTerrainEditor().brush().settings();
        ImGui::SliderFloat("Radius", &s.radius, 5.0f, 200.0f, "%.0f");
        ImGui::SliderFloat("Strength", &s.strength, 0.5f, 20.0f, "%.1f");
        ImGui::SliderFloat("Falloff", &s.falloff, 0.0f, 1.0f, "%.2f");

        ImGui::Separator();
        const char* paintModes[] = {"Paint", "Erase", "Replace Base"};
        int pm = static_cast<int>(paintMode_);
        if (ImGui::Combo("Paint Mode", &pm, paintModes, 3))
            paintMode_ = static_cast<PaintMode>(pm);

        ImGui::Separator();

        // Directory filter
        auto& browser = app.getAssetBrowser();
        auto& dirs = browser.getTextureDirectories();
        if (ImGui::BeginCombo("Zone", texDirIdx_ < 0 ? "All" :
                dirs[texDirIdx_].c_str())) {
            if (ImGui::Selectable("All", texDirIdx_ < 0)) texDirIdx_ = -1;
            for (int i = 0; i < static_cast<int>(dirs.size()); i++) {
                // Show just the zone name part
                std::string label = dirs[i];
                auto slash = label.rfind('\\');
                if (slash != std::string::npos) label = label.substr(slash + 1);
                if (ImGui::Selectable(label.c_str(), i == texDirIdx_))
                    texDirIdx_ = i;
            }
            ImGui::EndCombo();
        }

        ImGui::InputText("Filter", texFilterBuf_, sizeof(texFilterBuf_));
        std::string filter(texFilterBuf_);
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        float listHeight = ImGui::GetContentRegionAvail().y - 60;
        ImGui::BeginChild("TexList", ImVec2(0, listHeight), true);

        const auto& textures = browser.getTextures();
        int shown = 0;
        for (const auto& tex : textures) {
            if (texDirIdx_ >= 0 && tex.directory != dirs[texDirIdx_]) continue;
            if (!matchesFilter(tex.wowPath, filter)) continue;
            if (++shown > 500) { ImGui::Text("... %zu more (refine filter)", textures.size()); break; }

            bool selected = (tex.wowPath == selectedTexture_);
            if (ImGui::Selectable(tex.displayName.c_str(), selected)) {
                selectedTexture_ = tex.wowPath;
                app.getTexturePainter().setActiveTexture(tex.wowPath);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tex.wowPath.c_str());
        }
        ImGui::EndChild();

        if (!selectedTexture_.empty())
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Active: %s",
                               selectedTexture_.c_str());

        // Show textures on chunk under cursor
        auto& brush = app.getTerrainEditor().brush();
        if (brush.isActive()) {
            if (auto* terrain = app.getTerrainEditor().getTerrain()) {
                glm::vec3 bp = brush.getPosition();
                float tileNW_X = (32.0f - static_cast<float>(terrain->coord.y)) * 533.33333f;
                float tileNW_Y = (32.0f - static_cast<float>(terrain->coord.x)) * 533.33333f;
                int cy = static_cast<int>((tileNW_X - bp.x) / (533.33333f / 16.0f));
                int cx = static_cast<int>((tileNW_Y - bp.y) / (533.33333f / 16.0f));
                cx = std::clamp(cx, 0, 15);
                cy = std::clamp(cy, 0, 15);
                auto& chunk = terrain->chunks[cy * 16 + cx];
                if (!chunk.layers.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Chunk [%d,%d] layers:", cx, cy);
                    for (size_t li = 0; li < chunk.layers.size(); li++) {
                        uint32_t tid = chunk.layers[li].textureId;
                        std::string tname = (tid < terrain->textures.size()) ? terrain->textures[tid] : "?";
                        auto sl = tname.rfind('\\');
                        if (sl != std::string::npos) tname = tname.substr(sl + 1);
                        ImGui::BulletText("%s%s", li == 0 ? "[base] " : "", tname.c_str());
                    }
                }
            }
        }

        // Recent textures
        auto& recent = app.getTexturePainter().getRecentTextures();
        if (!recent.empty()) {
            ImGui::Separator();
            ImGui::Text("Recent:");
            for (int i = 0; i < static_cast<int>(recent.size()) && i < 6; i++) {
                std::string disp = recent[i];
                auto sl = disp.rfind('\\');
                if (sl != std::string::npos) disp = disp.substr(sl + 1);
                if (ImGui::SmallButton(disp.c_str())) {
                    selectedTexture_ = recent[i];
                    app.getTexturePainter().setActiveTexture(recent[i]);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", recent[i].c_str());
                if (i < 5 && i + 1 < static_cast<int>(recent.size())) ImGui::SameLine();
            }
        }
    }
    ImGui::End();
}

void EditorUI::renderObjectPanel(EditorApp& app) {
    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 550), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Object Placement")) {
        if (!app.hasTerrainLoaded()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Load or create terrain first");
            ImGui::End(); return;
        }

        auto& placer = app.getObjectPlacer();

        // Placement settings for new objects
        ImGui::Text("New Object Settings:");
        float rot = placer.getPlacementRotationY();
        if (ImGui::SliderFloat("Y Rotation", &rot, 0.0f, 360.0f, "%.0f deg"))
            placer.setPlacementRotationY(rot);
        bool randRot = placer.getRandomRotation();
        if (ImGui::Checkbox("Random Rotation", &randRot))
            placer.setRandomRotation(randRot);
        ImGui::SameLine();
        bool snap = placer.getSnapToGround();
        if (ImGui::Checkbox("Snap Ground", &snap))
            placer.setSnapToGround(snap);
        float scale = placer.getPlacementScale();
        if (ImGui::SliderFloat("Scale", &scale, 0.1f, 10.0f, "%.2f"))
            placer.setPlacementScale(scale);

        ImGui::Separator();
        ImGui::Checkbox("M2 Models", &showM2s_);
        ImGui::SameLine();
        ImGui::Checkbox("WMO Buildings", &showWMOs_);

        ImGui::InputText("Filter", objFilterBuf_, sizeof(objFilterBuf_));
        std::string filter(objFilterBuf_);
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        auto& browser = app.getAssetBrowser();
        float listHeight = ImGui::GetContentRegionAvail().y - 100;
        ImGui::BeginChild("ObjList", ImVec2(0, listHeight), true);

        int shown = 0;
        if (showM2s_) {
            for (const auto& m2 : browser.getM2Models()) {
                if (!matchesFilter(m2.wowPath, filter)) continue;
                if (++shown > 500) { ImGui::Text("... refine filter"); break; }

                bool selected = (m2.wowPath == placer.getActivePath() &&
                                 placer.getActiveType() == PlaceableType::M2);
                if (ImGui::Selectable(m2.displayName.c_str(), selected))
                    placer.setActivePath(m2.wowPath, PlaceableType::M2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", m2.wowPath.c_str());
            }
        }
        if (showWMOs_) {
            if (showM2s_ && shown > 0) ImGui::Separator();
            for (const auto& wmo : browser.getWMOs()) {
                if (!matchesFilter(wmo.wowPath, filter)) continue;
                if (++shown > 500) { ImGui::Text("... refine filter"); break; }

                bool selected = (wmo.wowPath == placer.getActivePath() &&
                                 placer.getActiveType() == PlaceableType::WMO);
                if (ImGui::Selectable(wmo.displayName.c_str(), selected))
                    placer.setActivePath(wmo.wowPath, PlaceableType::WMO);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", wmo.wowPath.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Text("Placed: %zu objects", placer.objectCount());
        if (placer.objectCount() > 0 && ImGui::CollapsingHeader("Object List")) {
            ImGui::BeginChild("ObjPlacedList", ImVec2(0, 100), true);
            for (int i = 0; i < static_cast<int>(placer.objectCount()); i++) {
                auto& o = const_cast<std::vector<PlacedObject>&>(placer.getObjects())[i];
                std::string disp = o.path;
                auto sl = disp.rfind('\\');
                if (sl != std::string::npos) disp = disp.substr(sl + 1);
                char lbl[128];
                std::snprintf(lbl, sizeof(lbl), "%s (%.0f,%.0f,%.0f)##obj%d",
                              disp.c_str(), o.position.x, o.position.y, o.position.z, i);
                if (ImGui::Selectable(lbl, o.selected)) {
                    placer.clearSelection();
                    // Select by creating a ray through the object position
                    rendering::Ray r;
                    r.origin = o.position + glm::vec3(0, 0, 1);
                    r.direction = glm::vec3(0, 0, -1);
                    placer.selectAt(r, 1000.0f);
                }
            }
            ImGui::EndChild();
        }
        if (auto* sel = placer.getSelected()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.9f, 0.3f, 1));
            ImGui::Text("Selected: %s", sel->path.c_str());
            ImGui::PopStyleColor();

            bool changed = false;
            changed |= ImGui::DragFloat3("Position", &sel->position.x, 1.0f);
            changed |= ImGui::DragFloat3("Rotation", &sel->rotation.x, 1.0f, 0.0f, 360.0f, "%.1f deg");
            changed |= ImGui::DragFloat("Obj Scale", &sel->scale, 0.05f, 0.1f, 50.0f, "%.2f");

            if (changed) app.markObjectsDirty();

            if (ImGui::Button("Snap Ground", ImVec2(75, 0)))
                app.snapSelectedToGround();
            ImGui::SameLine();
            if (ImGui::Button("Fly To", ImVec2(55, 0)))
                app.flyToSelected();
            ImGui::SameLine();
            if (ImGui::Button("Delete", ImVec2(55, 0))) placer.deleteSelected();
            if (ImGui::Button("Duplicate", ImVec2(100, 0))) {
                std::string dupPath = sel->path;
                glm::vec3 dupPos = sel->position + glm::vec3(10.0f, 10.0f, 0.0f);
                glm::vec3 dupRot = sel->rotation;
                float dupScale = sel->scale;
                auto dupType = sel->type;
                placer.clearSelection();
                placer.setActivePath(dupPath, dupType);
                placer.setPlacementScale(dupScale);
                placer.setPlacementRotationY(dupRot.y);
                placer.placeObject(dupPos);
                app.markObjectsDirty();
            }
            ImGui::SameLine();
            if (ImGui::Button("Deselect", ImVec2(100, 0)))
                placer.clearSelection();
        }

        ImGui::Separator();
        // Object scatter
        if (ImGui::CollapsingHeader("Scatter Objects")) {
            static int objScatterCount = 8;
            static float objScatterRadius = 60.0f;
            static float objMinScale = 0.8f;
            static float objMaxScale = 1.5f;
            ImGui::SliderInt("Count##objsc", &objScatterCount, 1, 50);
            ImGui::SliderFloat("Radius##objsc", &objScatterRadius, 10.0f, 300.0f);
            ImGui::DragFloatRange2("Scale##objsc", &objMinScale, &objMaxScale, 0.05f, 0.1f, 10.0f);
            auto& brush = app.getTerrainEditor().brush();
            if (ImGui::Button("Scatter at Cursor##obj", ImVec2(-1, 0))) {
                if (brush.isActive() && !placer.getActivePath().empty()) {
                    placer.scatter(brush.getPosition(), objScatterRadius,
                                   objScatterCount, objMinScale, objMaxScale);
                    app.markObjectsDirty();
                }
            }
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
                "Scatters selected model with random rotation/scale");
        }

        ImGui::Separator();
        // Bulk operations
        if (ImGui::CollapsingHeader("Bulk Operations")) {
            static float bulkRadius = 50.0f;
            ImGui::SliderFloat("Radius##bulk", &bulkRadius, 10.0f, 200.0f);
            auto& brush = app.getTerrainEditor().brush();
            if (ImGui::Button("Delete All in Radius", ImVec2(-1, 0)) && brush.isActive()) {
                auto& objs = const_cast<std::vector<PlacedObject>&>(placer.getObjects());
                glm::vec3 center = brush.getPosition();
                objs.erase(std::remove_if(objs.begin(), objs.end(),
                    [&](const PlacedObject& o) {
                        return glm::length(glm::vec2(o.position.x - center.x,
                                                     o.position.y - center.y)) < bulkRadius;
                    }), objs.end());
                app.markObjectsDirty();
                app.showToast("Deleted objects in radius");
            }
            if (ImGui::Button("Snap All to Ground", ImVec2(-1, 0))) {
                for (auto& o : const_cast<std::vector<PlacedObject>&>(placer.getObjects())) {
                    rendering::Ray r;
                    r.origin = o.position + glm::vec3(0, 0, 500);
                    r.direction = glm::vec3(0, 0, -1);
                    glm::vec3 hit;
                    if (app.getTerrainEditor().raycastTerrain(r, hit))
                        o.position.z = hit.z;
                }
                app.markObjectsDirty();
                app.showToast("All objects snapped to ground");
            }
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1), "Left-click: place | Ctrl+click: select");
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1), "G: move | R: rotate | T: scale | Del: remove");
    }
    ImGui::End();
}

void EditorUI::renderNpcPanel(EditorApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 400, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(390, 700), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("NPC / Monsters")) {
        if (!app.hasTerrainLoaded()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Load terrain first");
            ImGui::End(); return;
        }

        auto& spawner = app.getNpcSpawner();
        auto& presets = app.getNpcPresets();
        auto& tmpl = spawner.getTemplate();

        // ---- Creature Browser ----
        if (ImGui::CollapsingHeader("Creature Browser", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Category filter
            static int catIdx = -1;
            if (ImGui::BeginCombo("Category", catIdx < 0 ? "All" :
                    NpcPresets::getCategoryName(static_cast<CreatureCategory>(catIdx)))) {
                if (ImGui::Selectable("All", catIdx < 0)) catIdx = -1;
                for (int i = 0; i < static_cast<int>(CreatureCategory::COUNT); i++) {
                    auto cat = static_cast<CreatureCategory>(i);
                    auto& list = presets.getByCategory(cat);
                    if (list.empty()) continue;
                    char label[64];
                    std::snprintf(label, sizeof(label), "%s (%zu)",
                                  NpcPresets::getCategoryName(cat), list.size());
                    if (ImGui::Selectable(label, catIdx == i)) catIdx = i;
                }
                ImGui::EndCombo();
            }

            static char npcFilter[128] = "";
            ImGui::InputText("Search##npc", npcFilter, sizeof(npcFilter));
            std::string filter(npcFilter);
            std::transform(filter.begin(), filter.end(), filter.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            ImGui::BeginChild("CreatureList", ImVec2(0, 150), true);
            const auto& list = (catIdx < 0) ? presets.getPresets()
                               : presets.getByCategory(static_cast<CreatureCategory>(catIdx));
            int shown = 0;
            for (const auto& p : list) {
                if (!filter.empty()) {
                    std::string lower = p.name;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (lower.find(filter) == std::string::npos) continue;
                }
                if (++shown > 200) { ImGui::Text("... refine search"); break; }

                bool selected = (tmpl.modelPath == p.modelPath);
                if (ImGui::Selectable(p.name.c_str(), selected)) {
                    tmpl.name = p.name;
                    tmpl.modelPath = p.modelPath;
                    tmpl.level = p.defaultLevel;
                    tmpl.health = p.defaultHealth;
                    tmpl.hostile = p.defaultHostile;
                    tmpl.minDamage = 3 + p.defaultLevel * 2;
                    tmpl.maxDamage = 5 + p.defaultLevel * 3;
                    tmpl.armor = p.defaultLevel * 10;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", p.modelPath.c_str());
            }
            ImGui::EndChild();

            if (!tmpl.modelPath.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "Selected: %s", tmpl.name.c_str());
            }
        }

        // ---- Stats & Behavior ----
        if (ImGui::CollapsingHeader("Stats & Behavior", ImGuiTreeNodeFlags_DefaultOpen)) {
            static char nameBuf[128] = "";
            if (nameBuf[0] == '\0') std::strncpy(nameBuf, tmpl.name.c_str(), sizeof(nameBuf) - 1);
            if (ImGui::InputText("Name##tmpl", nameBuf, sizeof(nameBuf)))
                tmpl.name = nameBuf;

            ImGui::SliderFloat("Scale", &tmpl.scale, 0.5f, 10.0f, "%.1f");

            int lvl = tmpl.level;
            if (ImGui::SliderInt("Level", &lvl, 1, 83)) {
                tmpl.level = lvl;
                tmpl.health = 50 + lvl * 80;
                tmpl.minDamage = 3 + lvl * 2;
                tmpl.maxDamage = 5 + lvl * 3;
                tmpl.armor = lvl * 10;
            }

            int hp = tmpl.health;
            if (ImGui::InputInt("Health", &hp)) tmpl.health = std::max(1, hp);
            int mp = tmpl.mana;
            if (ImGui::InputInt("Mana", &mp)) tmpl.mana = std::max(0, mp);

            int dmin = tmpl.minDamage, dmax = tmpl.maxDamage;
            ImGui::InputInt("Min Dmg", &dmin); tmpl.minDamage = std::max(0, dmin);
            ImGui::InputInt("Max Dmg", &dmax); tmpl.maxDamage = std::max(0, dmax);
            int arm = tmpl.armor;
            if (ImGui::InputInt("Armor", &arm)) tmpl.armor = std::max(0, arm);

            const char* behaviors[] = {"Stationary", "Patrol", "Wander", "Scripted"};
            int bIdx = static_cast<int>(tmpl.behavior);
            if (ImGui::Combo("Behavior", &bIdx, behaviors, 4))
                tmpl.behavior = static_cast<CreatureBehavior>(bIdx);

            if (tmpl.behavior == CreatureBehavior::Wander)
                ImGui::SliderFloat("Wander Dist", &tmpl.wanderRadius, 1.0f, 100.0f);
            ImGui::SliderFloat("Aggro Range", &tmpl.aggroRadius, 0.0f, 100.0f);

            ImGui::Checkbox("Hostile", &tmpl.hostile);
            ImGui::SameLine(); ImGui::Checkbox("Questgiver", &tmpl.questgiver);
            ImGui::Checkbox("Vendor", &tmpl.vendor);
            ImGui::SameLine(); ImGui::Checkbox("Innkeeper", &tmpl.innkeeper);

            // Update nameBuf when preset selection changes it
            if (tmpl.name.c_str() != std::string(nameBuf))
                std::strncpy(nameBuf, tmpl.name.c_str(), sizeof(nameBuf) - 1);
        }

        ImGui::Separator();

        // ---- Spawned list ----
        if (ImGui::CollapsingHeader("Spawned Creatures", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%zu placed", spawner.spawnCount());
            ImGui::BeginChild("SpawnList", ImVec2(0, 100), true);
            for (int i = 0; i < static_cast<int>(spawner.spawnCount()); i++) {
                auto& s = spawner.getSpawns()[i];
                bool sel = (i == spawner.getSelectedIndex());
                char label[128];
                std::snprintf(label, sizeof(label), "%s Lv%u (%.0f,%.0f,%.0f)",
                              s.name.c_str(), s.level,
                              s.position.x, s.position.y, s.position.z);
                if (ImGui::Selectable(label, sel))
                    spawner.selectAt(s.position, 10000.0f);
            }
            ImGui::EndChild();
        }

        // ---- Selected creature editor ----
        if (auto* sel = spawner.getSelected()) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.9f, 0.3f, 1));
            ImGui::Text("Editing: %s", sel->name.c_str());
            ImGui::PopStyleColor();

            ImGui::DragFloat3("Pos##npc", &sel->position.x, 1.0f);
            ImGui::SliderFloat("Facing", &sel->orientation, 0.0f, 360.0f, "%.0f");
            int hp2 = sel->health; if (ImGui::InputInt("HP##s", &hp2)) sel->health = std::max(1, hp2);
            int lv2 = sel->level; if (ImGui::InputInt("Lv##s", &lv2)) sel->level = std::max(1, lv2);

            const char* beh2[] = {"Stationary", "Patrol", "Wander", "Scripted"};
            int bi2 = static_cast<int>(sel->behavior);
            if (ImGui::Combo("AI##s", &bi2, beh2, 4)) sel->behavior = static_cast<CreatureBehavior>(bi2);

            // Patrol path editing
            if (sel->behavior == CreatureBehavior::Patrol) {
                ImGui::Text("Patrol Points: %zu", sel->patrolPath.size());
                auto& brush = app.getTerrainEditor().brush();
                if (ImGui::Button("Add Point at Cursor##patrol", ImVec2(-1, 0))) {
                    if (brush.isActive()) {
                        PatrolPoint pp;
                        pp.position = brush.getPosition();
                        pp.waitTimeMs = 2000.0f;
                        sel->patrolPath.push_back(pp);
                    }
                }
                if (!sel->patrolPath.empty()) {
                    ImGui::BeginChild("PatrolList", ImVec2(0, 80), true);
                    for (int pi = 0; pi < static_cast<int>(sel->patrolPath.size()); pi++) {
                        auto& pp = sel->patrolPath[pi];
                        char lbl[64];
                        std::snprintf(lbl, sizeof(lbl), "P%d (%.0f,%.0f,%.0f) %.1fs",
                                      pi, pp.position.x, pp.position.y, pp.position.z,
                                      pp.waitTimeMs / 1000.0f);
                        ImGui::Text("%s", lbl);
                        ImGui::SameLine();
                        char delBtn[16]; std::snprintf(delBtn, sizeof(delBtn), "X##p%d", pi);
                        if (ImGui::SmallButton(delBtn))
                            sel->patrolPath.erase(sel->patrolPath.begin() + pi--);
                    }
                    ImGui::EndChild();
                    if (ImGui::Button("Clear Path##patrol"))
                        sel->patrolPath.clear();
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Fly To##npc", ImVec2(55, 0)))
                app.flyToSelected();
            ImGui::SameLine();
            if (ImGui::Button("Duplicate##npc", ImVec2(80, 0))) {
                CreatureSpawn copy = *sel;
                copy.position += glm::vec3(10, 10, 0);
                spawner.placeCreature(copy);
                app.markObjectsDirty();
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete##npc", ImVec2(80, 0))) spawner.removeCreature(spawner.getSelectedIndex());
            ImGui::SameLine();
            if (ImGui::Button("Desel.##npc", ImVec2(60, 0))) spawner.clearSelection();
        }

        ImGui::Separator();

        // Scatter tool
        if (ImGui::CollapsingHeader("Scatter Tool")) {
            static int scatterCount = 5;
            static float scatterRadius = 50.0f;
            ImGui::SliderInt("Count", &scatterCount, 1, 30);
            ImGui::SliderFloat("Radius##scatter", &scatterRadius, 10.0f, 200.0f);
            auto& brush = app.getTerrainEditor().brush();
            if (ImGui::Button("Scatter at Cursor", ImVec2(-1, 0))) {
                if (brush.isActive() && !tmpl.modelPath.empty()) {
                    spawner.scatter(tmpl, brush.getPosition(), scatterRadius, scatterCount);
                    app.markObjectsDirty();
                }
            }
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
                "Places %d copies in %.0f radius", scatterCount, scatterRadius);
        }

        ImGui::Separator();
        static char npcPath[256] = "output/creatures.json";
        ImGui::InputText("File##npc", npcPath, sizeof(npcPath));
        if (ImGui::Button("Save NPCs", ImVec2(100, 0))) spawner.saveToFile(npcPath);
        ImGui::SameLine();
        if (ImGui::Button("Load NPCs", ImVec2(100, 0))) {
            spawner.loadFromFile(npcPath);
            app.markObjectsDirty();
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1), "Click terrain to place selected creature");
    }
    ImGui::End();
}

void EditorUI::renderWaterPanel(EditorApp& app) {
    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 250), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Water")) {
        if (!app.hasTerrainLoaded()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Load or create terrain first");
            ImGui::End(); return;
        }

        auto& s = app.getTerrainEditor().brush().settings();
        ImGui::SliderFloat("Radius", &s.radius, 5.0f, 200.0f, "%.0f");

        float wh = app.getWaterHeight();
        if (ImGui::SliderFloat("Water Height", &wh, -200.0f, 500.0f, "%.1f"))
            app.setWaterHeight(wh);

        const char* types[] = {"Water", "Ocean", "Magma", "Slime"};
        int typeIdx = app.getWaterType();
        if (ImGui::Combo("Liquid Type", &typeIdx, types, 4))
            app.setWaterType(static_cast<uint16_t>(typeIdx));

        ImGui::Separator();
        if (ImGui::Button("Remove Water Under Brush", ImVec2(-1, 0))) {
            auto& brush = app.getTerrainEditor().brush();
            if (brush.isActive()) {
                app.getTerrainEditor().removeWater(brush.getPosition(), s.radius);
                app.getEditorCamera(); // trigger dirty
            }
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Left-click to place water");
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Rendered as translucent overlay");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1), "Left-click: place");
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1), "Ctrl+click: select");
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1), "Del: remove selected");
    }
    ImGui::End();
}

void EditorUI::renderContextMenu(EditorApp& app) {
    if (app.shouldOpenContextMenu()) {
        ImGui::OpenPopup("ObjectContextMenu");
        app.clearContextMenuFlag();
    }
    if (ImGui::BeginPopup("ObjectContextMenu")) {
        auto* objSel = app.getObjectPlacer().getSelected();
        auto* npcSel = app.getNpcSpawner().getSelected();
        if (!objSel && !npcSel) { ImGui::EndPopup(); return; }

        if (objSel) {
            std::string display = objSel->path;
            auto slash = display.rfind('\\');
            if (slash != std::string::npos) display = display.substr(slash + 1);
            ImGui::TextColored(ImVec4(1, 0.9f, 0.3f, 1), "%s", display.c_str());
        } else {
            ImGui::TextColored(ImVec4(1, 0.9f, 0.3f, 1), "%s (NPC)", npcSel->name.c_str());
        }
        ImGui::Separator();

        if (objSel) {
            if (ImGui::MenuItem("Move (G)"))
                app.startGizmoMode(TransformMode::Move);
            if (ImGui::MenuItem("Rotate (R)"))
                app.startGizmoMode(TransformMode::Rotate);
            if (ImGui::MenuItem("Scale (T)"))
                app.startGizmoMode(TransformMode::Scale);
            ImGui::Separator();
            if (ImGui::MenuItem("Snap to Ground"))
                app.snapSelectedToGround();
            if (ImGui::MenuItem("Fly To"))
                app.flyToSelected();
        }
        if (npcSel) {
            if (ImGui::MenuItem("Fly To"))
                app.flyToSelected();
            if (ImGui::MenuItem("Duplicate")) {
                CreatureSpawn copy = *npcSel;
                copy.position += glm::vec3(10, 10, 0);
                app.getNpcSpawner().placeCreature(copy);
                app.markObjectsDirty();
            }
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            if (objSel) { app.getObjectPlacer().deleteSelected(); }
            else { app.getNpcSpawner().removeCreature(app.getNpcSpawner().getSelectedIndex()); }
            app.markObjectsDirty();
        }
        if (ImGui::MenuItem("Deselect")) {
            app.getObjectPlacer().clearSelection();
            app.getNpcSpawner().clearSelection();
        }

        ImGui::EndPopup();
    }
}

void EditorUI::renderMinimap(EditorApp& app) {
    if (!app.hasTerrainLoaded()) return;
    auto* terrain = app.getTerrainEditor().getTerrain();
    if (!terrain) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 185, vp->Size.y - 210), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(175, 185), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    if (ImGui::Begin("Minimap", nullptr, ImGuiWindowFlags_NoScrollbar)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float cellW = avail.x / 16.0f;
        float cellH = (avail.y - 14) / 16.0f;
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Find height range
        float minH = 1e30f, maxH = -1e30f;
        for (int i = 0; i < 256; i++) {
            auto& c = terrain->chunks[i];
            if (!c.hasHeightMap()) continue;
            for (int v = 0; v < 145; v++) {
                float h = c.position[2] + c.heightMap.heights[v];
                minH = std::min(minH, h); maxH = std::max(maxH, h);
            }
        }
        float range = std::max(1.0f, maxH - minH);

        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++) {
                auto& c = terrain->chunks[cy * 16 + cx];
                float avgH = c.position[2];
                if (c.hasHeightMap()) {
                    float sum = 0;
                    for (int v = 0; v < 145; v++) sum += c.heightMap.heights[v];
                    avgH += sum / 145.0f;
                }
                float t = (avgH - minH) / range;

                // Color: low=blue, mid=green, high=brown/white
                float r, g, b;
                if (t < 0.3f) { r = 0.1f; g = 0.2f + t; b = 0.5f - t; }
                else if (t < 0.7f) { float tt = (t - 0.3f) / 0.4f; r = 0.1f + tt * 0.4f; g = 0.5f + tt * 0.2f; b = 0.1f; }
                else { float tt = (t - 0.7f) / 0.3f; r = 0.5f + tt * 0.3f; g = 0.7f - tt * 0.2f; b = 0.1f + tt * 0.5f; }

                ImVec2 p0(origin.x + cx * cellW, origin.y + cy * cellH);
                ImVec2 p1(p0.x + cellW - 1, p0.y + cellH - 1);
                dl->AddRectFilled(p0, p1, IM_COL32(
                    static_cast<int>(r*255), static_cast<int>(g*255),
                    static_cast<int>(b*255), 200));

                // Water indicator
                if (terrain->waterData[cy * 16 + cx].hasWater())
                    dl->AddRectFilled(p0, p1, IM_COL32(50, 100, 200, 100));
            }
        }

        // Draw objects as yellow dots
        float tileNW_X = (32.0f - static_cast<float>(terrain->coord.y)) * 533.33333f;
        float tileNW_Y = (32.0f - static_cast<float>(terrain->coord.x)) * 533.33333f;
        for (const auto& obj : app.getObjectPlacer().getObjects()) {
            float u = (tileNW_X - obj.position.x) / 533.33333f;
            float v = (tileNW_Y - obj.position.y) / 533.33333f;
            if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                ImVec2 pt(origin.x + v * avail.x, origin.y + u * (16 * cellH));
                dl->AddCircleFilled(pt, 2.0f, IM_COL32(255, 220, 50, 200));
            }
        }
        // Draw NPCs as red dots
        for (const auto& npc : app.getNpcSpawner().getSpawns()) {
            float u = (tileNW_X - npc.position.x) / 533.33333f;
            float v = (tileNW_Y - npc.position.y) / 533.33333f;
            if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                ImVec2 pt(origin.x + v * avail.x, origin.y + u * (16 * cellH));
                dl->AddCircleFilled(pt, 2.5f, npc.hostile ? IM_COL32(255, 60, 60, 200)
                                                           : IM_COL32(60, 200, 60, 200));
            }
        }

        ImGui::Dummy(ImVec2(avail.x, 16 * cellH));
        // Click minimap to move camera
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float mu = (mousePos.y - origin.y) / (16 * cellH);
            float mv = (mousePos.x - origin.x) / avail.x;
            if (mu >= 0 && mu <= 1 && mv >= 0 && mv <= 1) {
                float wx = tileNW_X - mu * 533.33333f;
                float wy = tileNW_Y - mv * 533.33333f;
                app.getEditorCamera().setPosition(glm::vec3(wx, wy,
                    app.getEditorCamera().getCamera().getPosition().z));
            }
        }

        // Camera indicator as white cross
        auto camPos = app.getEditorCamera().getCamera().getPosition();
        float camU = (tileNW_X - camPos.x) / 533.33333f;
        float camV = (tileNW_Y - camPos.y) / 533.33333f;
        if (camU >= 0 && camU <= 1 && camV >= 0 && camV <= 1) {
            ImVec2 cp(origin.x + camV * avail.x, origin.y + camU * (16 * cellH));
            dl->AddLine(ImVec2(cp.x - 3, cp.y), ImVec2(cp.x + 3, cp.y), IM_COL32(255,255,255,220), 2);
            dl->AddLine(ImVec2(cp.x, cp.y - 3), ImVec2(cp.x, cp.y + 3), IM_COL32(255,255,255,220), 2);
        }

        ImGui::Dummy(ImVec2(avail.x, 16 * cellH));
        // Legend
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        ImVec2 legPos = ImGui::GetCursorScreenPos();
        dl2->AddCircleFilled(ImVec2(legPos.x + 5, legPos.y + 5), 3, IM_COL32(255, 220, 50, 200));
        dl2->AddCircleFilled(ImVec2(legPos.x + 45, legPos.y + 5), 3, IM_COL32(255, 60, 60, 200));
        dl2->AddCircleFilled(ImVec2(legPos.x + 100, legPos.y + 5), 3, IM_COL32(60, 200, 60, 200));
        ImGui::Text("   Obj      Hostile   Friendly  + Cam");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorUI::renderPropertiesPanel(EditorApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 280, 90), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(270, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Info")) {
        auto* tr = app.getTerrainRenderer();
        if (tr && tr->getChunkCount() > 0) {
            ImGui::Text("Map: %s [%d, %d]", app.getLoadedMap().c_str(),
                        app.getLoadedTileX(), app.getLoadedTileY());
            ImGui::Text("Chunks: %d  Tris: %d", tr->getChunkCount(), tr->getTriangleCount());
            ImGui::Text("Objects: %zu  NPCs: %zu",
                        app.getObjectPlacer().objectCount(),
                        app.getNpcSpawner().spawnCount());
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No terrain loaded");
        }
        if (auto* t = app.getTerrainEditor().getTerrain()) {
            ImGui::Text("Textures: %zu", t->textures.size());
            int waterChunks = 0;
            int holeChunks = 0;
            for (int i = 0; i < 256; i++) {
                if (t->waterData[i].hasWater()) waterChunks++;
                if (t->chunks[i].holes) holeChunks++;
            }
            if (waterChunks > 0) ImGui::Text("Water chunks: %d", waterChunks);
            if (holeChunks > 0) ImGui::Text("Hole chunks: %d", holeChunks);
        }

        // M2 render diagnostics
        if (auto* m2r = app.getM2Renderer()) {
            ImGui::Text("M2: %u models, %u instances, %u draws",
                        m2r->getModelCount(), m2r->getInstanceCount(),
                        m2r->getDrawCallCount());
        }

        ImGui::Separator();
        auto pos = app.getEditorCamera().getCamera().getPosition();
        ImGui::Text("Camera: %.0f, %.0f, %.0f", pos.x, pos.y, pos.z);
        ImGui::Text("Speed: %.0f (Shift+scroll)", app.getEditorCamera().getSpeed());

        // Cursor world position
        auto& brush = app.getTerrainEditor().brush();
        if (brush.isActive()) {
            auto bp = brush.getPosition();
            ImGui::Text("Cursor: %.1f, %.1f, %.1f", bp.x, bp.y, bp.z);
        }

        ImGui::Separator();
        if (app.getTerrainEditor().hasUnsavedChanges())
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "* Unsaved (Ctrl+S to save)");
        else
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1), "Saved");
    }
    ImGui::End();
}

void EditorUI::renderStatusBar(EditorApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float h = 24.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        const char* ms[] = {"Sculpt", "Paint", "Objects", "Water", "NPCs"};
        const char* m = ms[static_cast<int>(app.getMode())];
        if (app.hasTerrainLoaded()) {
            ImGui::Text("[%s] %s [%d,%d]%s", m, app.getLoadedMap().c_str(),
                        app.getLoadedTileX(), app.getLoadedTileY(),
                        app.getTerrainEditor().hasUnsavedChanges() ? " *" : "");
            ImGui::SameLine(vp->Size.x * 0.4f);
            ImGui::Text("Obj:%zu NPC:%zu",
                        app.getObjectPlacer().objectCount(),
                        app.getNpcSpawner().spawnCount());
        } else {
            ImGui::Text("[%s] Wowee World Editor", m);
        }
        ImGui::SameLine(vp->Size.x - 120);
        ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace editor
} // namespace wowee
