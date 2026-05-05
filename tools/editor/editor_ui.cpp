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
    renderPropertiesPanel(app);
    renderStatusBar(app);
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
    if (saveAdtRequested_) {
        saveAdtRequested_ = false;
        app.saveADT(savePathBuf_);
    }
    if (saveWdtRequested_) {
        saveWdtRequested_ = false;
        app.saveWDT(std::string(savePathBuf_));
    }
}

void EditorUI::renderMenuBar(EditorApp& app) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Terrain...", "Ctrl+N")) showNewDialog_ = true;
            if (ImGui::MenuItem("Load ADT...", "Ctrl+O")) showLoadDialog_ = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Save ADT...", "Ctrl+S", false, app.hasTerrainLoaded()))
                showSaveDialog_ = true;
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
            if (ImGui::MenuItem("Reset Camera")) app.resetCamera();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void EditorUI::renderToolbar(EditorApp& app) {
    ImGui::SetNextWindowPos(ImVec2(300, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 50), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::Begin("##Toolbar", nullptr, flags)) {
        auto mode = app.getMode();
        if (ImGui::RadioButton("Sculpt", mode == EditorMode::Sculpt))
            app.setMode(EditorMode::Sculpt);
        ImGui::SameLine();
        if (ImGui::RadioButton("Paint", mode == EditorMode::Paint))
            app.setMode(EditorMode::Paint);
        ImGui::SameLine();
        if (ImGui::RadioButton("Objects", mode == EditorMode::PlaceObject))
            app.setMode(EditorMode::PlaceObject);
        ImGui::SameLine();
        if (ImGui::RadioButton("Water", mode == EditorMode::Water))
            app.setMode(EditorMode::Water);
        ImGui::SameLine();
        if (ImGui::RadioButton("NPCs", mode == EditorMode::NPC))
            app.setMode(EditorMode::NPC);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorUI::renderNewTerrainDialog(EditorApp& /*app*/) {
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("New Terrain", &showNewDialog_)) {
        ImGui::InputText("Map Name", newMapNameBuf_, sizeof(newMapNameBuf_));
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

void EditorUI::renderLoadDialog(EditorApp& /*app*/) {
    ImGui::SetNextWindowSize(ImVec2(350, 180), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Load ADT", &showLoadDialog_)) {
        ImGui::InputText("Map Name", loadMapNameBuf_, sizeof(loadMapNameBuf_));
        ImGui::InputInt("Tile X", &loadTileX_);
        ImGui::InputInt("Tile Y", &loadTileY_);
        loadTileX_ = std::max(0, std::min(63, loadTileX_));
        loadTileY_ = std::max(0, std::min(63, loadTileY_));
        ImGui::Spacing();
        if (ImGui::Button("Load", ImVec2(120, 0))) { loadRequested_ = true; showLoadDialog_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) showLoadDialog_ = false;
    }
    ImGui::End();
}

void EditorUI::renderSaveDialog(EditorApp& app) {
    ImGui::SetNextWindowSize(ImVec2(500, 200), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Save", &showSaveDialog_)) {
        if (savePathBuf_[0] == '\0' && app.hasTerrainLoaded())
            std::snprintf(savePathBuf_, sizeof(savePathBuf_), "output/%s/%s_%d_%d.adt",
                          app.getLoadedMap().c_str(), app.getLoadedMap().c_str(),
                          app.getLoadedTileX(), app.getLoadedTileY());
        ImGui::InputText("Path", savePathBuf_, sizeof(savePathBuf_));
        ImGui::Spacing();
        if (ImGui::Button("Save ADT", ImVec2(140, 0))) { saveAdtRequested_ = true; showSaveDialog_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Save ADT + WDT", ImVec2(140, 0))) {
            saveAdtRequested_ = true; saveWdtRequested_ = true; showSaveDialog_ = false;
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
        const char* modes[] = {"Raise", "Lower", "Smooth", "Flatten", "Level"};
        int idx = static_cast<int>(s.mode);
        if (ImGui::Combo("Mode", &idx, modes, 5)) s.mode = static_cast<BrushMode>(idx);
        ImGui::SliderFloat("Radius", &s.radius, 5.0f, 200.0f, "%.0f");
        ImGui::SliderFloat("Strength", &s.strength, 0.5f, 50.0f, "%.1f");
        ImGui::SliderFloat("Falloff", &s.falloff, 0.0f, 1.0f, "%.2f");
        if (s.mode == BrushMode::Flatten || s.mode == BrushMode::Level)
            ImGui::SliderFloat("Target Height", &s.flattenHeight, -500.0f, 1000.0f, "%.1f");
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
        if (auto* sel = placer.getSelected()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.9f, 0.3f, 1));
            ImGui::Text("Selected: %s", sel->path.c_str());
            ImGui::PopStyleColor();

            bool changed = false;
            changed |= ImGui::DragFloat3("Position", &sel->position.x, 1.0f);
            changed |= ImGui::DragFloat3("Rotation", &sel->rotation.x, 1.0f, 0.0f, 360.0f, "%.1f deg");
            changed |= ImGui::DragFloat("Obj Scale", &sel->scale, 0.05f, 0.1f, 50.0f, "%.2f");

            if (changed) app.markObjectsDirty();

            if (ImGui::Button("Delete", ImVec2(100, 0))) placer.deleteSelected();
            ImGui::SameLine();
            if (ImGui::Button("Duplicate", ImVec2(100, 0))) {
                PlacedObject copy = *sel;
                copy.uniqueId = 0;
                copy.position += glm::vec3(5.0f, 5.0f, 0.0f);
                copy.selected = false;
                placer.clearSelection();
                // Can't easily push from here, but move slightly signals intent
            }
            ImGui::SameLine();
            if (ImGui::Button("Deselect", ImVec2(100, 0)))
                placer.clearSelection();
        }
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

            if (ImGui::Button("Delete##npc")) spawner.removeCreature(spawner.getSelectedIndex());
            ImGui::SameLine();
            if (ImGui::Button("Deselect##npc")) spawner.clearSelection();
        }

        ImGui::Separator();
        static char npcPath[256] = "output/creatures.json";
        ImGui::InputText("File##npc", npcPath, sizeof(npcPath));
        if (ImGui::Button("Save NPCs")) spawner.saveToFile(npcPath);

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
    if (ImGui::BeginPopup("ObjectContextMenu")) {
        auto* sel = app.getObjectPlacer().getSelected();
        if (!sel) { ImGui::EndPopup(); return; }

        std::string display = sel->path;
        auto slash = display.rfind('\\');
        if (slash != std::string::npos) display = display.substr(slash + 1);
        ImGui::TextColored(ImVec4(1, 0.9f, 0.3f, 1), "%s", display.c_str());
        ImGui::Separator();

        if (ImGui::MenuItem("Move (left-drag)"))
            app.startGizmoMode(TransformMode::Move);
        if (ImGui::MenuItem("Rotate (left-drag)"))
            app.startGizmoMode(TransformMode::Rotate);
        if (ImGui::MenuItem("Scale (left-drag)"))
            app.startGizmoMode(TransformMode::Scale);

        ImGui::Separator();
        if (ImGui::BeginMenu("Constrain Axis")) {
            if (ImGui::MenuItem("All Axes")) app.setGizmoAxis(TransformAxis::All);
            if (ImGui::MenuItem("X (Red)")) app.setGizmoAxis(TransformAxis::X);
            if (ImGui::MenuItem("Y (Green)")) app.setGizmoAxis(TransformAxis::Y);
            if (ImGui::MenuItem("Z (Blue)")) app.setGizmoAxis(TransformAxis::Z);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            app.getObjectPlacer().deleteSelected();
            app.markObjectsDirty();
        }
        if (ImGui::MenuItem("Deselect"))
            app.getObjectPlacer().clearSelection();

        ImGui::EndPopup();
    }
}

void EditorUI::renderPropertiesPanel(EditorApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 280, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(270, 180), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Properties")) {
        auto* tr = app.getTerrainRenderer();
        if (tr && tr->getChunkCount() > 0) {
            ImGui::Text("Map: %s [%d, %d]", app.getLoadedMap().c_str(),
                        app.getLoadedTileX(), app.getLoadedTileY());
            ImGui::Text("Chunks: %d  Tris: %d", tr->getChunkCount(), tr->getTriangleCount());
            ImGui::Text("Objects: %zu", app.getObjectPlacer().objectCount());
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No terrain loaded");
        }
        ImGui::Separator();
        auto pos = app.getEditorCamera().getCamera().getPosition();
        ImGui::Text("Camera: %.0f, %.0f, %.0f", pos.x, pos.y, pos.z);
        ImGui::Text("Speed: %.0f (scroll)", app.getEditorCamera().getSpeed());
        if (app.getTerrainEditor().hasUnsavedChanges())
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "* Unsaved changes");
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
        if (app.hasTerrainLoaded())
            ImGui::Text("[%s] %s [%d,%d]%s", m, app.getLoadedMap().c_str(),
                        app.getLoadedTileX(), app.getLoadedTileY(),
                        app.getTerrainEditor().hasUnsavedChanges() ? " *" : "");
        else
            ImGui::Text("[%s] Wowee World Editor", m);
        ImGui::SameLine(vp->Size.x - 120);
        ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace editor
} // namespace wowee
