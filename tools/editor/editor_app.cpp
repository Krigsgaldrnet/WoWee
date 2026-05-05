#include "editor_app.hpp"
#include "adt_writer.hpp"
#include "zone_manifest.hpp"
#include "content_pack.hpp"
#include "wowee_terrain.hpp"
#include "texture_exporter.hpp"
#include "dbc_exporter.hpp"
#include "pipeline/wowee_model.hpp"
#include "pipeline/wowee_building.hpp"
#include "core/coordinates.hpp"
#include "rendering/vk_context.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_set>

namespace wowee {
namespace editor {

EditorApp::EditorApp() = default;
EditorApp::~EditorApp() { shutdown(); }

bool EditorApp::initialize(const std::string& dataPath) {
    dataPath_ = dataPath;

    core::WindowConfig wc;
    wc.title = "Wowee World Editor";
    wc.width = 1600;
    wc.height = 900;
    window_ = std::make_unique<core::Window>(wc);
    if (!window_->initialize()) {
        LOG_ERROR("Failed to initialize window");
        return false;
    }

    assetManager_ = std::make_unique<pipeline::AssetManager>();
    if (!assetManager_->initialize(dataPath)) {
        LOG_ERROR("Failed to initialize asset manager with path: ", dataPath);
        return false;
    }

    initImGui();

    auto* vkCtx = window_->getVkContext();
    camera_.getCamera().setAspectRatio(window_->getAspectRatio());
    camera_.setPosition(glm::vec3(0.0f, 0.0f, 300.0f));
    camera_.setYawPitch(0.0f, -30.0f);

    if (!viewport_.initialize(vkCtx, assetManager_.get(), &camera_.getCamera())) {
        LOG_ERROR("Failed to initialize editor viewport");
        return false;
    }

    assetBrowser_.initialize(assetManager_.get());
    npcPresets_.initialize(assetManager_.get());

    LOG_INFO("Editor initialized (data: ", dataPath, ")");
    return true;
}

void EditorApp::run() {
    auto lastTime = std::chrono::steady_clock::now();

    while (!window_->shouldClose()) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        dt = std::min(dt, 0.1f);

        processEvents();

        auto* vkCtx = window_->getVkContext();
        if (vkCtx->isSwapchainDirty()) {
            int w = window_->getWidth();
            int h = window_->getHeight();
            if (w > 0 && h > 0) {
                (void)vkCtx->recreateSwapchain(w, h);
                camera_.getCamera().setAspectRatio(static_cast<float>(w) / h);
            }
        }

        camera_.update(dt);
        updateTerrainEditing(dt);

        // Handle pending UI actions
        ui_.processActions(*this);

        updateToasts(dt);

        // Auto-save
        if (autoSaveEnabled_ && terrain_.isLoaded() && terrainEditor_.hasUnsavedChanges()) {
            autoSaveTimer_ += dt;
            if (autoSaveTimer_ >= autoSaveInterval_) {
                autoSaveTimer_ = 0.0f;
                quickSave();
                LOG_INFO("Auto-saved zone");
            }
        }

        // Refresh dirty terrain chunks
        refreshDirtyChunks();

        // Track object and NPC counts separately
        size_t objCount = objectPlacer_.objectCount();
        size_t npcCount = npcSpawner_.spawnCount();
        bool objChanged = (objCount != lastObjCount_);
        bool npcChanged = (npcCount != lastNpcCount_) || objectsDirty_;

        if (npcChanged) {
            // NPC markers are cheap — always update
            viewport_.updateNpcMarkers(npcSpawner_.getSpawns());
            lastNpcCount_ = npcCount;
        }

        if (objChanged || objectsDirty_) {
            // Full M2 rebuild only when placed objects change
            objectsDirty_ = false;
            lastObjCount_ = objCount;
            if (objCount > 0 || npcCount > 0) {
                vkDeviceWaitIdle(vkCtx->getDevice());
                viewport_.rebuildObjects(objectPlacer_.getObjects(), npcSpawner_.getSpawns());
            }
            lastNpcCount_ = npcCount; // sync after rebuild
        }

        // Show gizmo arrows on selected object
        auto& gizmo = viewport_.getGizmo();
        if (auto* sel = objectPlacer_.getSelected()) {
            gizmo.setTarget(sel->position, sel->scale);
        } else {
            gizmo.setMode(TransformMode::None);
        }

        uint32_t imageIndex = 0;
        VkCommandBuffer cmd = vkCtx->beginFrame(imageIndex);
        if (cmd == VK_NULL_HANDLE) continue;

        // Update M2 animations AFTER beginFrame (so getCurrentFrame is correct)
        viewport_.update(dt);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ui_.render(*this);

        ImGui::Render();

        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass = vkCtx->getImGuiRenderPass();
        rpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[imageIndex];
        rpInfo.renderArea.offset = {0, 0};
        rpInfo.renderArea.extent = vkCtx->getSwapchainExtent();

        VkClearValue clearValues[4]{};
        float cr, cg, cb;
        viewport_.getClearColor(cr, cg, cb);
        clearValues[0].color = {{cr, cg, cb, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};
        rpInfo.clearValueCount = 2;
        rpInfo.pClearValues = clearValues;

        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        auto ext = vkCtx->getSwapchainExtent();
        VkViewport vp{};
        vp.width = static_cast<float>(ext.width);
        vp.height = static_cast<float>(ext.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D scissor{};
        scissor.extent = ext;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        viewport_.render(cmd);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        vkCmdEndRenderPass(cmd);
        vkCtx->endFrame(cmd, imageIndex);
    }
}

void EditorApp::shutdown() {
    if (!window_) return;
    auto* vkCtx = window_->getVkContext();
    if (vkCtx) vkDeviceWaitIdle(vkCtx->getDevice());

    viewport_.shutdown();
    shutdownImGui();

    if (assetManager_) {
        assetManager_->shutdown();
        assetManager_.reset();
    }
    window_.reset();
}

void EditorApp::processEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);

        if (event.type == SDL_QUIT) {
            window_->setShouldClose(true);
            return;
        }

        if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                window_->setSize(event.window.data1, event.window.data2);
                window_->getVkContext()->markSwapchainDirty();
            }
        }

        auto& io = ImGui::GetIO();

        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            if (event.type == SDL_KEYDOWN) {
                auto sc = event.key.keysym.scancode;
                if (sc == SDL_SCANCODE_F3) setWireframe(!isWireframe());
                if (sc == SDL_SCANCODE_F5) saveBookmark("");
                if (sc == SDL_SCANCODE_HOME) centerOnTerrain();
                // Number keys switch modes (when not typing in ImGui)
                if (!io.WantCaptureKeyboard) {
                    if (sc == SDL_SCANCODE_1) setMode(EditorMode::Sculpt);
                    if (sc == SDL_SCANCODE_2) setMode(EditorMode::Paint);
                    if (sc == SDL_SCANCODE_3) setMode(EditorMode::PlaceObject);
                    if (sc == SDL_SCANCODE_4) setMode(EditorMode::Water);
                    if (sc == SDL_SCANCODE_5) setMode(EditorMode::NPC);
                    if (sc == SDL_SCANCODE_6) setMode(EditorMode::Quest);
                }
                // F1 handled by UI (showHelp_ toggle)
                // Transform shortcuts (Blender-style)
                if (objectPlacer_.getSelected()) {
                    if (sc == SDL_SCANCODE_G) startGizmoMode(TransformMode::Move);
                    if (sc == SDL_SCANCODE_R) startGizmoMode(TransformMode::Rotate);
                    if (sc == SDL_SCANCODE_T) startGizmoMode(TransformMode::Scale);
                    if (sc == SDL_SCANCODE_X) setGizmoAxis(TransformAxis::X);
                    if (sc == SDL_SCANCODE_Y) setGizmoAxis(TransformAxis::Y);
                    if (sc == SDL_SCANCODE_ESCAPE) {
                        viewport_.getGizmo().endDrag();
                        viewport_.getGizmo().setMode(TransformMode::None);
                        objectPlacer_.clearSelection();
                    }
                }
                if (sc == SDL_SCANCODE_DELETE) {
                    if (objectPlacer_.getSelected()) {
                        objectPlacer_.deleteSelected();
                        objectsDirty_ = true;
                    } else if (npcSpawner_.getSelected()) {
                        npcSpawner_.removeCreature(npcSpawner_.getSelectedIndex());
                        objectsDirty_ = true;
                    }
                }
                if (sc == SDL_SCANCODE_S && (event.key.keysym.mod & KMOD_CTRL))
                    quickSave();
                if (sc == SDL_SCANCODE_N && (event.key.keysym.mod & KMOD_CTRL))
                    ui_.openNewTerrainDialog();
                if (sc == SDL_SCANCODE_O && (event.key.keysym.mod & KMOD_CTRL))
                    ui_.openLoadDialog();
                if (sc == SDL_SCANCODE_Z && (event.key.keysym.mod & KMOD_CTRL)) {
                    bool isRedo = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                    if (isRedo) {
                        // Ctrl+Shift+Z = Redo (sculpt only for now)
                        if (terrainEditor_.history().canRedo()) {
                            terrainEditor_.redo();
                            showToast("Redo");
                        }
                    } else {
                        // Ctrl+Z = Undo
                        if (mode_ == EditorMode::PlaceObject || mode_ == EditorMode::NPC) {
                            if (objectPlacer_.canUndoPlace()) {
                                objectPlacer_.undoLastPlace();
                                objectsDirty_ = true;
                                showToast("Undo placement");
                            }
                        } else if (terrainEditor_.history().canUndo()) {
                            terrainEditor_.undo();
                            showToast("Undo");
                        }
                    }
                }
            }
            if (!io.WantCaptureKeyboard)
                camera_.processKeyEvent(event.key);
        }

        if (event.type == SDL_MOUSEMOTION && !io.WantCaptureMouse) {
            // Gizmo drag takes priority over camera
            auto& giz = viewport_.getGizmo();
            if (event.motion.state & SDL_BUTTON_MMASK) {
                // Middle mouse = orbit around brush/terrain point
                auto& brush = terrainEditor_.brush();
                glm::vec3 pivot = brush.isActive() ? brush.getPosition() : camera_.getCamera().getPosition() + camera_.getCamera().getForward() * 100.0f;
                camera_.processMiddleMouseMotion(event.motion.xrel, event.motion.yrel, pivot);
            } else if (giz.isDragging()) {
                auto ext = window_->getVkContext()->getSwapchainExtent();
                giz.updateDrag(glm::vec2(static_cast<float>(event.motion.x),
                                          static_cast<float>(event.motion.y)),
                               camera_.getCamera(),
                               static_cast<float>(ext.width),
                               static_cast<float>(ext.height));
                // Apply transform to selected object
                if (auto* sel = objectPlacer_.getSelected()) {
                    if (giz.getMode() == TransformMode::Move) {
                        sel->position += giz.getMoveDelta();
                        giz.beginDrag(glm::vec2(event.motion.x, event.motion.y));
                    } else if (giz.getMode() == TransformMode::Rotate) {
                        sel->rotation += giz.getRotateDelta();
                        giz.beginDrag(glm::vec2(event.motion.x, event.motion.y));
                    } else if (giz.getMode() == TransformMode::Scale) {
                        sel->scale = std::max(0.1f, sel->scale + giz.getScaleDelta());
                        giz.beginDrag(glm::vec2(event.motion.x, event.motion.y));
                    }
                    giz.setTarget(sel->position, sel->scale);
                    objectsDirty_ = true;
                }
            } else {
                camera_.processMouseMotion(event.motion.xrel, event.motion.yrel);
            }
        }

        if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) && !io.WantCaptureMouse) {
            // Right-click on selected objects = context menu
            if (event.button.button == SDL_BUTTON_RIGHT && event.type == SDL_MOUSEBUTTONDOWN) {
                auto& giz = viewport_.getGizmo();
                if (giz.isDragging()) {
                    giz.endDrag();
                    giz.setMode(TransformMode::None);
                } else if (objectPlacer_.getSelected() || npcSpawner_.getSelected()) {
                    openContextMenu_ = true;
                } else {
                    camera_.processMouseButton(event.button);
                }
            } else if (event.button.button == SDL_BUTTON_RIGHT && event.type == SDL_MOUSEBUTTONUP) {
                if (!objectPlacer_.getSelected() && !npcSpawner_.getSelected())
                    camera_.processMouseButton(event.button);
            } else {
                // Only pass to camera if gizmo not active
                auto& giz = viewport_.getGizmo();
                if (!giz.isDragging())
                    camera_.processMouseButton(event.button);
            }

            // Left click
            if (event.button.button == SDL_BUTTON_LEFT && terrain_.isLoaded()) {
                auto& giz = viewport_.getGizmo();
                // End gizmo drag on left click
                if (giz.isDragging() && event.type == SDL_MOUSEBUTTONDOWN) {
                    giz.endDrag();
                    giz.setMode(TransformMode::None);
                } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                    // Ctrl+click = select object (any mode)
                    if ((event.key.keysym.mod & KMOD_CTRL) || (SDL_GetModState() & KMOD_CTRL)) {
                        auto ext = window_->getVkContext()->getSwapchainExtent();
                        rendering::Ray ray = camera_.getCamera().screenToWorldRay(
                            static_cast<float>(event.button.x),
                            static_cast<float>(event.button.y),
                            static_cast<float>(ext.width),
                            static_cast<float>(ext.height));
                        objectPlacer_.selectAt(ray, 200.0f);
                    } else if (mode_ == EditorMode::NPC) {
                        auto ext = window_->getVkContext()->getSwapchainExtent();
                        rendering::Ray ray = camera_.getCamera().screenToWorldRay(
                            static_cast<float>(event.button.x),
                            static_cast<float>(event.button.y),
                            static_cast<float>(ext.width),
                            static_cast<float>(ext.height));
                        glm::vec3 hitPos;
                        if (terrainEditor_.raycastTerrain(ray, hitPos)) {
                            auto& tmpl = npcSpawner_.getTemplate();
                            tmpl.position = hitPos;
                            npcSpawner_.placeCreature(tmpl);
                            objectsDirty_ = true;
                        }
                    } else if (mode_ == EditorMode::Water) {
                        painting_ = true;
                    } else if (mode_ == EditorMode::PlaceObject) {
                        // Raycast now at click time for accurate placement
                        auto ext = window_->getVkContext()->getSwapchainExtent();
                        rendering::Ray ray = camera_.getCamera().screenToWorldRay(
                            static_cast<float>(event.button.x),
                            static_cast<float>(event.button.y),
                            static_cast<float>(ext.width),
                            static_cast<float>(ext.height));
                        glm::vec3 hitPos;
                        if (terrainEditor_.raycastTerrain(ray, hitPos)) {
                            objectPlacer_.placeObject(hitPos);
                            objectsDirty_ = true;
                        }
                    } else {
                        painting_ = true;
                        if (mode_ == EditorMode::Sculpt)
                            terrainEditor_.beginStroke();
                    }
                } else if (event.type == SDL_MOUSEBUTTONUP) {
                    painting_ = false;
                    if (mode_ == EditorMode::Sculpt)
                        terrainEditor_.endStroke();
                }
            }

            // Middle click = select object
            if (event.button.button == SDL_BUTTON_MIDDLE && event.type == SDL_MOUSEBUTTONDOWN) {
                if (mode_ == EditorMode::PlaceObject && terrain_.isLoaded()) {
                    auto ext = window_->getVkContext()->getSwapchainExtent();
                    auto& io2 = ImGui::GetIO();
                    rendering::Ray ray = camera_.getCamera().screenToWorldRay(
                        io2.MousePos.x, io2.MousePos.y,
                        static_cast<float>(ext.width), static_cast<float>(ext.height));
                    objectPlacer_.selectAt(ray);
                }
            }
        }

        if (event.type == SDL_MOUSEWHEEL && !io.WantCaptureMouse)
            camera_.processMouseWheel(event.wheel.y, (SDL_GetModState() & KMOD_SHIFT) != 0);
    }
}

void EditorApp::updateTerrainEditing(float dt) {
    if (!terrain_.isLoaded()) return;

    // Update brush position from mouse cursor
    auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        float mx = io.MousePos.x;
        float my = io.MousePos.y;
        auto ext = window_->getVkContext()->getSwapchainExtent();

        rendering::Ray ray = camera_.getCamera().screenToWorldRay(
            mx, my, static_cast<float>(ext.width), static_cast<float>(ext.height));

        glm::vec3 hitPos;
        if (terrainEditor_.raycastTerrain(ray, hitPos)) {
            terrainEditor_.brush().setPosition(hitPos);
            terrainEditor_.brush().setActive(true);

            // Ghost preview for object/NPC placement
            if (mode_ == EditorMode::PlaceObject && !objectPlacer_.getActivePath().empty()) {
                viewport_.setGhostPreview(
                    objectPlacer_.getActivePath(), hitPos,
                    glm::vec3(0, objectPlacer_.getPlacementRotationY(), 0),
                    objectPlacer_.getPlacementScale());
            } else if (mode_ == EditorMode::NPC && !npcSpawner_.getTemplate().modelPath.empty()) {
                viewport_.setGhostPreview(
                    npcSpawner_.getTemplate().modelPath, hitPos,
                    glm::vec3(0, 0, 0), npcSpawner_.getTemplate().scale);
            } else if (mode_ != EditorMode::PlaceObject && mode_ != EditorMode::NPC) {
                viewport_.clearGhostPreview();
            }

            // Brush circle indicator for sculpt/paint/water modes
            if (mode_ == EditorMode::Sculpt || mode_ == EditorMode::Paint || mode_ == EditorMode::Water) {
                viewport_.setBrushIndicator(hitPos, terrainEditor_.brush().settings().radius, true);
            } else {
                viewport_.setBrushIndicator(hitPos, 0, false);
            }

            if (painting_ && terrainEditor_.brush().settings().mode == BrushMode::Flatten) {
                static bool flattenSet = false;
                if (!flattenSet) {
                    terrainEditor_.brush().settings().flattenHeight = hitPos.z;
                    flattenSet = true;
                }
                if (!io.MouseDown[0]) flattenSet = false;
            }
        } else {
            terrainEditor_.brush().setActive(false);
            viewport_.setBrushIndicator({}, 0, false);
            viewport_.clearGhostPreview();
        }
    }

    if (painting_ && terrainEditor_.brush().isActive()) {
        if (mode_ == EditorMode::Sculpt) {
            terrainEditor_.applyBrush(dt);
        } else if (mode_ == EditorMode::Paint) {
            auto& brush = terrainEditor_.brush();
            auto paintMode = ui_.getPaintMode();
            std::vector<int> modified;

            if (paintMode == PaintMode::Erase) {
                modified = texturePainter_.erase(
                    brush.getPosition(), brush.settings().radius,
                    brush.settings().strength * dt * 0.5f, brush.settings().falloff);
            } else if (paintMode == PaintMode::ReplaceBase) {
                // Replace base texture of chunks under brush
                auto& texPath = texturePainter_.getActiveTexture();
                if (!texPath.empty()) {
                    // Ensure texture is in list
                    uint32_t texId = 0;
                    for (uint32_t i = 0; i < terrain_.textures.size(); i++) {
                        if (terrain_.textures[i] == texPath) { texId = i; goto found; }
                    }
                    terrain_.textures.push_back(texPath);
                    texId = static_cast<uint32_t>(terrain_.textures.size() - 1);
                    found:
                    for (int ci = 0; ci < 256; ci++) {
                        auto& chunk = terrain_.chunks[ci];
                        if (!chunk.hasHeightMap() || chunk.layers.empty()) continue;
                        glm::vec3 cpos = terrainEditor_.brush().getPosition();
                        // Rough distance check
                        auto vpos = glm::vec3(chunk.position[1], chunk.position[0], chunk.position[2]);
                        if (glm::length(glm::vec2(vpos.x - cpos.x, vpos.y - cpos.y)) < brush.settings().radius + 40.0f) {
                            chunk.layers[0].textureId = texId;
                            modified.push_back(ci);
                        }
                    }
                }
            } else {
                modified = texturePainter_.paint(
                    brush.getPosition(), brush.settings().radius,
                    brush.settings().strength * dt * 0.5f, brush.settings().falloff);
            }

            if (!modified.empty()) {
                auto mesh = terrainEditor_.regenerateMesh();
                viewport_.clearTerrain();
                viewport_.loadTerrain(mesh, terrain_.textures, loadedTileX_, loadedTileY_);
            }
        } else if (mode_ == EditorMode::Water) {
            auto& brush = terrainEditor_.brush();
            terrainEditor_.setWaterLevel(brush.getPosition(), brush.settings().radius,
                                         waterHeight_, waterType_);
            viewport_.updateWater(terrain_, loadedTileX_, loadedTileY_);
        }
    }
}

void EditorApp::refreshDirtyChunks() {
    auto dirty = terrainEditor_.consumeDirtyChunks();
    if (dirty.empty()) return;

    // Recalculate normals for modified chunks (better lighting)
    terrainEditor_.recalcNormals(dirty);

    // Regenerate full mesh and reload terrain
    auto mesh = terrainEditor_.regenerateMesh();
    viewport_.clearTerrain();
    viewport_.loadTerrain(mesh, terrain_.textures, loadedTileX_, loadedTileY_);
}

void EditorApp::loadADT(const std::string& mapName, int tileX, int tileY) {
    std::ostringstream path;
    path << "World\\Maps\\" << mapName << "\\" << mapName
         << "_" << tileX << "_" << tileY << ".adt";

    LOG_INFO("Loading ADT: ", path.str());

    auto adtData = assetManager_->readFile(path.str());
    if (adtData.empty()) {
        LOG_ERROR("ADT file not found: ", path.str());
        return;
    }

    terrain_ = pipeline::ADTLoader::load(adtData);
    if (!terrain_.isLoaded()) {
        LOG_ERROR("Failed to parse ADT: ", path.str());
        return;
    }

    // Override internal coords with what we know from the filename
    // (instanced maps have arbitrary internal coord values)
    terrain_.coord = {tileX, tileY};

    terrainEditor_.setTerrain(&terrain_);
    terrainEditor_.history().clear();
    texturePainter_.setTerrain(&terrain_);
    objectPlacer_.setTerrain(&terrain_);

    auto mesh = pipeline::TerrainMeshGenerator::generate(terrain_);
    if (mesh.validChunkCount == 0) {
        LOG_ERROR("ADT has no valid terrain chunks");
        showToast("Error: no valid terrain data in this tile");
        return;
    }
    viewport_.clearTerrain();
    if (!viewport_.loadTerrain(mesh, terrain_.textures, tileX, tileY)) {
        LOG_ERROR("Failed to upload terrain to GPU (", mesh.validChunkCount, " chunks)");
        showToast("Error: terrain upload failed");
        return;
    }

    loadedMap_ = mapName;
    loadedTileX_ = tileX;
    loadedTileY_ = tileY;

    // Position camera at terrain center using actual chunk positions
    if (mesh.validChunkCount > 0) {
        auto& firstChunk = mesh.chunks[0];
        auto& lastChunk = mesh.chunks[255];
        float cx = (firstChunk.worldX + lastChunk.worldX) * 0.5f;
        float cy = (firstChunk.worldY + lastChunk.worldY) * 0.5f;
        float cz = firstChunk.worldZ + 300.0f;
        camera_.setPosition(glm::vec3(cx, cy, cz));
    } else {
        float centerX = (32.0f - tileY) * 533.33333f - 8.0f * 533.33333f / 16.0f;
        float centerY = (32.0f - tileX) * 533.33333f - 8.0f * 533.33333f / 16.0f;
        camera_.setPosition(glm::vec3(centerX, centerY, 400.0f));
    }
    camera_.setYawPitch(0.0f, -45.0f);

    // Import doodad/WMO placements from the ADT itself
    // ADT positions are in ADT coordinate space — convert to render coords
    for (const auto& dp : terrain_.doodadPlacements) {
        if (dp.nameId < terrain_.doodadNames.size()) {
            PlacedObject obj;
            obj.type = PlaceableType::M2;
            obj.path = terrain_.doodadNames[dp.nameId];
            obj.position = core::coords::adtToWorld(dp.position[0], dp.position[1], dp.position[2]);
            obj.rotation = glm::vec3(dp.rotation[0], dp.rotation[1], dp.rotation[2]);
            obj.scale = static_cast<float>(dp.scale) / 1024.0f;
            obj.uniqueId = dp.uniqueId;
            objectPlacer_.getObjects().push_back(obj);
        }
    }
    for (const auto& wp : terrain_.wmoPlacements) {
        if (wp.nameId < terrain_.wmoNames.size()) {
            PlacedObject obj;
            obj.type = PlaceableType::WMO;
            obj.path = terrain_.wmoNames[wp.nameId];
            obj.position = core::coords::adtToWorld(wp.position[0], wp.position[1], wp.position[2]);
            obj.rotation = glm::vec3(wp.rotation[0], wp.rotation[1], wp.rotation[2]);
            obj.scale = 1.0f;
            obj.uniqueId = wp.uniqueId;
            objectPlacer_.getObjects().push_back(obj);
        }
    }
    if (!terrain_.doodadPlacements.empty() || !terrain_.wmoPlacements.empty()) {
        objectsDirty_ = true;
        showToast("Imported " + std::to_string(terrain_.doodadPlacements.size()) +
                  " doodads + " + std::to_string(terrain_.wmoPlacements.size()) + " WMOs");
        LOG_INFO("Imported ", terrain_.doodadPlacements.size(), " doodads + ",
                 terrain_.wmoPlacements.size(), " WMOs from ADT");
    }

    LOG_INFO("ADT loaded: ", mapName, " [", tileX, ",", tileY, "]");

    // Try loading objects/NPCs from output directory if they exist
    std::string outBase = "output/" + mapName;
    if (objectPlacer_.loadFromFile(outBase + "/objects.json"))
        showToast("Loaded " + std::to_string(objectPlacer_.objectCount()) + " objects");
    if (npcSpawner_.loadFromFile(outBase + "/creatures.json"))
        showToast("Loaded " + std::to_string(npcSpawner_.spawnCount()) + " NPCs");
    if (objectPlacer_.objectCount() > 0 || npcSpawner_.spawnCount() > 0)
        objectsDirty_ = true;
}

void EditorApp::createNewTerrain(const std::string& mapName, int tileX, int tileY, float baseHeight, Biome biome) {
    terrain_ = TerrainEditor::createBlankTerrain(tileX, tileY, baseHeight, biome);
    // Clear previous state
    clearAllObjects();

    terrainEditor_.setTerrain(&terrain_);
    terrainEditor_.history().clear();
    texturePainter_.setTerrain(&terrain_);
    objectPlacer_.setTerrain(&terrain_);

    auto mesh = pipeline::TerrainMeshGenerator::generate(terrain_);
    viewport_.clearTerrain();
    viewport_.loadTerrain(mesh, terrain_.textures, tileX, tileY);

    loadedMap_ = mapName;
    loadedTileX_ = tileX;
    loadedTileY_ = tileY;
    lastObjCount_ = 0;
    lastNpcCount_ = 0;
    objectsDirty_ = false;

    float centerX = (32.0f - tileY) * 533.33333f - 8.0f * 533.33333f / 16.0f;
    float centerY = (32.0f - tileX) * 533.33333f - 8.0f * 533.33333f / 16.0f;
    camera_.setPosition(glm::vec3(centerX, centerY, baseHeight + 300.0f));
    camera_.setYawPitch(0.0f, -45.0f);

    LOG_INFO("New terrain created: ", mapName, " [", tileX, ",", tileY, "] base=", baseHeight);
}

void EditorApp::saveADT(const std::string& path) {
    if (!terrain_.isLoaded()) {
        LOG_ERROR("No terrain to save");
        return;
    }
    objectPlacer_.syncToTerrain();
    ADTWriter::write(terrain_, path);
    terrainEditor_.markSaved();
}

void EditorApp::saveWDT(const std::string& path) {
    if (loadedMap_.empty()) return;
    ADTWriter::writeWDT(loadedMap_, loadedTileX_, loadedTileY_, path);
}

void EditorApp::exportZone(const std::string& outputDir) {
    if (!terrain_.isLoaded() || loadedMap_.empty()) return;

    std::string base = outputDir + "/" + loadedMap_;

    // Save ADT
    std::string adtPath = base + "/" + loadedMap_ + "_" +
                          std::to_string(loadedTileX_) + "_" +
                          std::to_string(loadedTileY_) + ".adt";
    saveADT(adtPath);

    // Save WDT
    std::string wdtPath = base + "/" + loadedMap_ + ".wdt";
    saveWDT(wdtPath);

    // Save creature spawns
    if (npcSpawner_.spawnCount() > 0) {
        std::string npcPath = base + "/creatures.json";
        npcSpawner_.saveToFile(npcPath);
    }

    // Save quests
    if (questEditor_.questCount() > 0) {
        std::string questPath = base + "/quests.json";
        questEditor_.saveToFile(questPath);
    }

    // Update WDT with additional tiles from adjacent exports
    // (future: scan output dir for existing ADTs and include all in WDT)

    // Save placed objects
    if (objectPlacer_.objectCount() > 0) {
        std::string objPath = base + "/objects.json";
        objectPlacer_.saveToFile(objPath);
    }

    // Convert placed M2 objects to WOM open format
    if (objectPlacer_.objectCount() > 0) {
        std::unordered_set<std::string> convertedModels;
        for (const auto& obj : objectPlacer_.getObjects()) {
            if (obj.type == PlaceableType::M2 && !convertedModels.count(obj.path)) {
                auto wom = pipeline::WoweeModelLoader::fromM2(obj.path, assetManager_.get());
                if (wom.isValid()) {
                    std::string womPath = obj.path;
                    std::replace(womPath.begin(), womPath.end(), '\\', '/');
                    auto dot = womPath.rfind('.');
                    if (dot != std::string::npos) womPath = womPath.substr(0, dot);
                    pipeline::WoweeModelLoader::save(wom, base + "/models/" + womPath);
                    convertedModels.insert(obj.path);
                }
            }
        }
        if (!convertedModels.empty())
            LOG_INFO("Converted ", convertedModels.size(), " M2 models to WOM");
    }

    // Convert placed WMO buildings to WOB open format
    if (objectPlacer_.objectCount() > 0) {
        std::unordered_set<std::string> convertedWMOs;
        for (const auto& obj : objectPlacer_.getObjects()) {
            if (obj.type == PlaceableType::WMO && !convertedWMOs.count(obj.path)) {
                // Create a placeholder WOB (full WMO→WOB conversion needs group loading)
                pipeline::WoweeBuilding bld;
                bld.name = obj.path;
                std::string wobPath = obj.path;
                std::replace(wobPath.begin(), wobPath.end(), '\\', '/');
                auto dot = wobPath.rfind('.');
                if (dot != std::string::npos) wobPath = wobPath.substr(0, dot);
                pipeline::WoweeBuildingLoader::save(bld, base + "/buildings/" + wobPath);
                convertedWMOs.insert(obj.path);
            }
        }
        if (!convertedWMOs.empty())
            LOG_INFO("Created ", convertedWMOs.size(), " WOB building placeholders");
    }

    // Export used textures as PNG (open format replacement for BLP)
    auto usedTextures = TextureExporter::collectUsedTextures(terrain_);
    if (!usedTextures.empty()) {
        int exported = TextureExporter::exportTexturesAsPng(
            assetManager_.get(), usedTextures, base + "/textures");
        LOG_INFO("Exported ", exported, " textures as PNG");
    }

    // Export zone-relevant DBCs as JSON (open format replacement for DBC)
    DBCExporter::exportZoneDBCs(assetManager_.get(), base + "/data");

    // Export open terrain format alongside ADT
    std::string openBase = base + "/" + loadedMap_ + "_" +
                           std::to_string(loadedTileX_) + "_" + std::to_string(loadedTileY_);
    WoweeTerrain::exportOpen(terrain_, openBase, loadedTileX_, loadedTileY_);

    // Write zone info README
    {
        std::ofstream readme(base + "/README.txt");
        if (readme) {
            readme << "Zone: " << loadedMap_ << "\n";
            readme << "Tile: [" << loadedTileX_ << ", " << loadedTileY_ << "]\n";
            readme << "Objects: " << objectPlacer_.objectCount() << "\n";
            readme << "NPCs: " << npcSpawner_.spawnCount() << "\n";
            readme << "Quests: " << questEditor_.questCount() << "\n";
            readme << "Created with Wowee World Editor v0.3.0\n\n";
            readme << "Files:\n";
            readme << "  zone.json       - Zone manifest (for client)\n";
            readme << "  " << loadedMap_ << ".wdt   - Map definition\n";
            readme << "  " << loadedMap_ << "_" << loadedTileX_ << "_" << loadedTileY_ << ".adt - Terrain tile\n";
            if (objectPlacer_.objectCount() > 0) readme << "  objects.json    - Placed M2/WMO objects\n";
            if (npcSpawner_.spawnCount() > 0) readme << "  creatures.json  - NPC/monster spawns\n";
        }
    }

    // Write zone manifest (for client loading)
    ZoneManifest manifest;
    manifest.mapName = loadedMap_;
    manifest.displayName = loadedMap_;
    manifest.tiles.push_back({loadedTileX_, loadedTileY_});
    manifest.hasCreatures = (npcSpawner_.spawnCount() > 0);
    manifest.baseHeight = terrain_.chunks[0].position[2];
    manifest.save(base + "/zone.json");

    lastSavePath_ = outputDir;

    // Count exported files
    int fileCount = 2; // ADT + WDT always
    fileCount += 2; // WOT + WHM always
    fileCount += 1; // zone.json always
    fileCount += 1; // README always
    if (objectPlacer_.objectCount() > 0) fileCount++;
    if (npcSpawner_.spawnCount() > 0) fileCount++;
    if (questEditor_.questCount() > 0) fileCount++;

    // Validate open format completeness
    auto validation = ContentPacker::validateZone(base);
    int score = validation.openFormatScore();
    showToast("Exported " + std::to_string(fileCount) + " files (" +
              std::to_string(score) + "/5 open format)");
    LOG_INFO("Zone exported to: ", base, " (", fileCount, " files, open score: ",
             score, "/5 — ", validation.summary(), ")");
}

void EditorApp::exportContentPack(const std::string& destPath) {
    if (!terrain_.isLoaded()) return;
    // Save zone first
    std::string dir = lastSavePath_.empty() ? "output" : lastSavePath_;
    exportZone(dir);
    // Pack into WCP
    ContentPackInfo info;
    info.name = loadedMap_;
    info.author = "Kelsi Davis";
    info.description = "Custom zone created with Wowee World Editor";
    info.mapId = 9000;
    if (ContentPacker::packZone(dir, loadedMap_, destPath, info))
        showToast("Content pack exported: " + destPath);
    else
        showToast("Failed to create content pack");
}

void EditorApp::exportOpenFormat(const std::string& basePath) {
    if (!terrain_.isLoaded()) return;
    std::string base = basePath + "/" + loadedMap_ + "/" + loadedMap_ + "_" +
                       std::to_string(loadedTileX_) + "_" + std::to_string(loadedTileY_);
    if (WoweeTerrain::exportOpen(terrain_, base, loadedTileX_, loadedTileY_))
        showToast("Open format exported (.wot + .whm)");
    else
        showToast("Open format export failed");
}

void EditorApp::quickSave() {
    if (!terrain_.isLoaded()) return;
    std::string dir = lastSavePath_.empty() ? "output" : lastSavePath_;
    exportZone(dir);
}

void EditorApp::requestQuit() {
    window_->setShouldClose(true);
}

void EditorApp::showToast(const std::string& msg, float duration) {
    toasts_.push_back({msg, duration});
}

void EditorApp::updateToasts(float dt) {
    for (auto& t : toasts_) t.timer -= dt;
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                  [](const Toast& t) { return t.timer <= 0; }), toasts_.end());
}

void EditorApp::setSkyPreset(int preset) {
    switch (preset) {
        case 0: // Day
            viewport_.setClearColor(0.4f, 0.6f, 0.9f);
            viewport_.setLightDir(glm::normalize(glm::vec3(0.5f, -1.0f, 0.8f)));
            break;
        case 1: // Dusk
            viewport_.setClearColor(0.6f, 0.3f, 0.2f);
            viewport_.setLightDir(glm::normalize(glm::vec3(0.8f, -0.3f, 0.1f)));
            break;
        case 2: // Night
            viewport_.setClearColor(0.05f, 0.05f, 0.12f);
            viewport_.setLightDir(glm::normalize(glm::vec3(0.2f, -0.5f, 0.8f)));
            break;
    }
}

void EditorApp::startGizmoMode(TransformMode mode) {
    auto& giz = viewport_.getGizmo();
    giz.setMode(mode);
    auto& io = ImGui::GetIO();
    giz.beginDrag(glm::vec2(io.MousePos.x, io.MousePos.y));
}

void EditorApp::setGizmoAxis(TransformAxis axis) {
    viewport_.getGizmo().setAxis(axis);
    if (auto* sel = objectPlacer_.getSelected())
        viewport_.getGizmo().setTarget(sel->position, sel->scale);
}

void EditorApp::saveBookmark(const std::string& name) {
    CameraBookmark bm;
    bm.pos = camera_.getCamera().getPosition();
    bm.yaw = 0; bm.pitch = 0; // EditorCamera doesn't expose these directly
    bm.name = name.empty() ? ("Bookmark " + std::to_string(bookmarks_.size() + 1)) : name;
    bookmarks_.push_back(bm);
}

void EditorApp::loadBookmark(int index) {
    if (index < 0 || index >= static_cast<int>(bookmarks_.size())) return;
    camera_.setPosition(bookmarks_[index].pos);
}

void EditorApp::addAdjacentTile(int offsetX, int offsetY) {
    if (!terrain_.isLoaded()) return;
    int newX = loadedTileX_ + offsetX;
    int newY = loadedTileY_ + offsetY;
    if (newX < 0 || newX > 63 || newY < 0 || newY > 63) return;

    // Create a blank tile adjacent to current
    auto adj = TerrainEditor::createBlankTerrain(newX, newY, terrain_.chunks[0].position[2],
                                                  Biome::Grassland);
    // Stitch edges: copy border heights from current terrain to adjacent
    // (This is a simplified version — full multi-tile needs a different architecture)
    LOG_INFO("Adjacent tile created at [", newX, ",", newY, "] (not yet rendered in viewport)");
    ADTWriter::write(adj, "output/" + loadedMap_ + "/" + loadedMap_ + "_" +
                     std::to_string(newX) + "_" + std::to_string(newY) + ".adt");
}

void EditorApp::flyToSelected() {
    auto* sel = objectPlacer_.getSelected();
    if (sel) {
        camera_.setPosition(sel->position + glm::vec3(0, 0, 30));
        return;
    }
    auto* npc = npcSpawner_.getSelected();
    if (npc) {
        camera_.setPosition(npc->position + glm::vec3(0, 0, 30));
    }
}

void EditorApp::generateCompleteZone() {
    if (!terrain_.isLoaded()) return;
    showToast("Generating zone...");

    // Step 0: Reset first for clean slate
    terrainEditor_.resetToFlat();

    // Step 1: Apply noise
    terrainEditor_.applyNoise(0.005f, 30.0f, 4, 42);

    // Step 2: Smooth
    terrainEditor_.smoothEntireTile(3);

    // Step 3: Recalc normals for slope paint
    std::vector<int> allChunks;
    for (int i = 0; i < 256; i++) allChunks.push_back(i);
    terrainEditor_.recalcNormals(allChunks);

    // Step 4: Auto-paint by height
    std::vector<TexturePainter::HeightBand> bands = {
        {90.0f, "Tileset\\Tanaris\\TanarisSandBase01.blp"},
        {110.0f, "Tileset\\Elwynn\\ElwynnGrassBase.blp"},
        {140.0f, "Tileset\\Barrens\\BarrensRock01.blp"},
        {99999.0f, "Tileset\\Expansion02\\Dragonblight\\DragonblightFreshSmoothSnowA.blp"}
    };
    texturePainter_.autoPaintByHeight(bands);

    // Step 5: Slope paint (rock on cliffs)
    texturePainter_.autoPaintBySlope(0.4f, "Tileset\\Desolace\\DesolaceRock01.blp");

    // Step 6: Add detail roughness
    terrainEditor_.addDetailNoise(1.5f, 0.08f, 77);

    // Step 6b: Final normal recalculation after detail noise
    terrainEditor_.recalcNormals(allChunks);

    // Step 7: Fill low areas with water and smooth beaches
    float waterLevel = terrain_.chunks[0].position[2] + 5.0f;
    terrainEditor_.fillWater(waterLevel, 0);
    terrainEditor_.smoothBeaches(waterLevel, 12.0f);

    // Refresh
    auto mesh = terrainEditor_.regenerateMesh();
    viewport_.clearTerrain();
    viewport_.loadTerrain(mesh, terrain_.textures, loadedTileX_, loadedTileY_);

    showToast("Zone generated!");
}

void EditorApp::clearAllObjects() {
    vkDeviceWaitIdle(window_->getVkContext()->getDevice());
    objectPlacer_.clearAll();
    npcSpawner_.clearSelection();
    npcSpawner_.getSpawns().clear();
    viewport_.clearObjects();
    viewport_.updateNpcMarkers({});
    terrainEditor_.history().clear();
    lastObjCount_ = 0;
    lastNpcCount_ = 0;
    objectsDirty_ = false;
    showToast("All objects and NPCs cleared");
}

void EditorApp::centerOnTerrain() {
    if (!terrain_.isLoaded()) return;
    auto mesh = terrainEditor_.regenerateMesh();
    if (mesh.validChunkCount > 0) {
        float cx = (mesh.chunks[0].worldX + mesh.chunks[255].worldX) * 0.5f;
        float cy = (mesh.chunks[0].worldY + mesh.chunks[255].worldY) * 0.5f;
        camera_.setPosition(glm::vec3(cx, cy, terrain_.chunks[0].position[2] + 300.0f));
    }
    camera_.setYawPitch(0.0f, -45.0f);
    showToast("Camera centered on terrain");
}

void EditorApp::snapSelectedToGround() {
    auto* sel = objectPlacer_.getSelected();
    if (!sel || !terrain_.isLoaded()) return;

    // Cast ray straight down from object position
    rendering::Ray ray;
    ray.origin = sel->position + glm::vec3(0, 0, 500);
    ray.direction = glm::vec3(0, 0, -1);
    glm::vec3 hitPos;
    if (terrainEditor_.raycastTerrain(ray, hitPos)) {
        sel->position.z = hitPos.z;
        objectsDirty_ = true;
    }
}

void EditorApp::resetCamera() {
    camera_.setPosition(glm::vec3(0.0f, 0.0f, 300.0f));
    camera_.setYawPitch(0.0f, -30.0f);
}

void EditorApp::setWireframe(bool enabled) {
    viewport_.setWireframe(enabled);
}

bool EditorApp::isWireframe() const {
    return viewport_.isWireframe();
}

rendering::TerrainRenderer* EditorApp::getTerrainRenderer() {
    return viewport_.getTerrainRenderer();
}

void EditorApp::initImGui() {
    auto* vkCtx = window_->getVkContext();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "wowee_editor_layout.ini";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.95f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.18f, 0.25f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.24f, 0.28f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.35f, 0.50f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.24f, 0.36f, 1.00f);

    ImGui_ImplSDL2_InitForVulkan(window_->getSDLWindow());

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_1;
    initInfo.Instance = vkCtx->getInstance();
    initInfo.PhysicalDevice = vkCtx->getPhysicalDevice();
    initInfo.Device = vkCtx->getDevice();
    initInfo.QueueFamily = vkCtx->getGraphicsQueueFamily();
    initInfo.Queue = vkCtx->getGraphicsQueue();
    initInfo.DescriptorPool = vkCtx->getImGuiDescriptorPool();
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = vkCtx->getSwapchainImageCount();
    initInfo.PipelineInfoMain.RenderPass = vkCtx->getImGuiRenderPass();
    initInfo.PipelineInfoMain.MSAASamples = vkCtx->getMsaaSamples();

    ImGui_ImplVulkan_Init(&initInfo);
    imguiInitialized_ = true;
}

void EditorApp::shutdownImGui() {
    if (!imguiInitialized_) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized_ = false;
}

} // namespace editor
} // namespace wowee
