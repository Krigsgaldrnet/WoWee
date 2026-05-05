#include "editor_app.hpp"
#include "adt_writer.hpp"
#include "rendering/vk_context.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <chrono>
#include <sstream>

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

        // Refresh dirty terrain chunks
        refreshDirtyChunks();

        // Rebuild object visuals when object list changes
        size_t objCount = objectPlacer_.objectCount() + npcSpawner_.spawnCount();
        if (objectsDirty_ || objCount != lastObjectCount_) {
            objectsDirty_ = false;
            lastObjectCount_ = objCount;
            vkDeviceWaitIdle(window_->getVkContext()->getDevice());
            viewport_.rebuildObjects(objectPlacer_.getObjects(), npcSpawner_.getSpawns());
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
        clearValues[0].color = {{0.15f, 0.15f, 0.2f, 1.0f}};
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
                if (sc == SDL_SCANCODE_Z && (event.key.keysym.mod & KMOD_CTRL)) {
                    if (event.key.keysym.mod & KMOD_SHIFT)
                        terrainEditor_.redo();
                    else
                        terrainEditor_.undo();
                }
            }
            if (!io.WantCaptureKeyboard)
                camera_.processKeyEvent(event.key);
        }

        if (event.type == SDL_MOUSEMOTION && !io.WantCaptureMouse) {
            // Gizmo drag takes priority over camera
            auto& giz = viewport_.getGizmo();
            if (giz.isDragging()) {
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
            // Right-click context menu on selected objects
            if (event.button.button == SDL_BUTTON_RIGHT && event.type == SDL_MOUSEBUTTONDOWN) {
                auto& giz = viewport_.getGizmo();
                if (giz.isDragging()) {
                    giz.endDrag();
                    giz.setMode(TransformMode::None);
                } else if (objectPlacer_.getSelected()) {
                    openContextMenu_ = true;
                } else {
                    camera_.processMouseButton(event.button);
                }
            } else {
                camera_.processMouseButton(event.button);
            }

            // Left click
            if (event.button.button == SDL_BUTTON_LEFT && terrain_.isLoaded()) {
                // End gizmo drag on click release
                auto& giz = viewport_.getGizmo();
                if (giz.isDragging() && event.type == SDL_MOUSEBUTTONUP) {
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
            camera_.processMouseWheel(event.wheel.y);
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

    terrainEditor_.setTerrain(&terrain_);
    terrainEditor_.history().clear();
    texturePainter_.setTerrain(&terrain_);
    objectPlacer_.setTerrain(&terrain_);

    auto mesh = pipeline::TerrainMeshGenerator::generate(terrain_);
    viewport_.clearTerrain();
    if (!viewport_.loadTerrain(mesh, terrain_.textures, tileX, tileY)) {
        LOG_ERROR("Failed to upload terrain to GPU");
        return;
    }

    loadedMap_ = mapName;
    loadedTileX_ = tileX;
    loadedTileY_ = tileY;

    float centerX = (32.0f - tileY) * 533.33333f - 8.0f * 533.33333f / 16.0f;
    float centerY = (32.0f - tileX) * 533.33333f - 8.0f * 533.33333f / 16.0f;
    camera_.setPosition(glm::vec3(centerX, centerY, 400.0f));
    camera_.setYawPitch(0.0f, -45.0f);

    LOG_INFO("ADT loaded: ", mapName, " [", tileX, ",", tileY, "]");
}

void EditorApp::createNewTerrain(const std::string& mapName, int tileX, int tileY, float baseHeight, Biome biome) {
    terrain_ = TerrainEditor::createBlankTerrain(tileX, tileY, baseHeight, biome);
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

void EditorApp::requestQuit() {
    window_->setShouldClose(true);
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
