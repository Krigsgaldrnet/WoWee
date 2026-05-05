#include "editor_viewport.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "core/logger.hpp"
#include <cstring>
#include <unordered_map>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace editor {

EditorViewport::EditorViewport() = default;
EditorViewport::~EditorViewport() { shutdown(); }

bool EditorViewport::initialize(rendering::VkContext* ctx, pipeline::AssetManager* am,
                                 rendering::Camera* cam) {
    vkCtx_ = ctx;
    assetManager_ = am;
    camera_ = cam;

    if (!createPerFrameResources()) return false;

    terrainRenderer_ = std::make_unique<rendering::TerrainRenderer>();
    if (!terrainRenderer_->initialize(ctx, perFrameSetLayout_, am)) {
        LOG_ERROR("Failed to initialize terrain renderer");
        return false;
    }
    terrainRenderer_->setFogEnabled(false);

    m2Renderer_ = std::make_unique<rendering::M2Renderer>();
    if (!m2Renderer_->initialize(ctx, perFrameSetLayout_, am)) {
        LOG_WARNING("M2 renderer init failed — object rendering disabled");
        m2Renderer_.reset();
    } else {
        m2Renderer_->setForceNoCull(true);
    }

    wmoRenderer_ = std::make_unique<rendering::WMORenderer>();
    if (!wmoRenderer_->initialize(ctx, perFrameSetLayout_, am)) {
        LOG_WARNING("WMO renderer init failed — building rendering disabled");
        wmoRenderer_.reset();
    }

    waterRenderer_.initialize(ctx, ctx->getImGuiRenderPass(), perFrameSetLayout_);
    markerRenderer_.initialize(ctx, ctx->getImGuiRenderPass(), perFrameSetLayout_);
    gizmo_.initialize(ctx, ctx->getImGuiRenderPass(), perFrameSetLayout_);

    LOG_INFO("Editor viewport initialized");
    return true;
}

void EditorViewport::shutdown() {
    if (!vkCtx_) return;
    vkDeviceWaitIdle(vkCtx_->getDevice());

    gizmo_.shutdown();
    markerRenderer_.shutdown();
    waterRenderer_.shutdown();

    if (wmoRenderer_) { wmoRenderer_->shutdown(); wmoRenderer_.reset(); }
    if (m2Renderer_) { m2Renderer_->shutdown(); m2Renderer_.reset(); }
    if (terrainRenderer_) { terrainRenderer_->shutdown(); terrainRenderer_.reset(); }

    destroyPerFrameResources();
    vkCtx_ = nullptr;
}

bool EditorViewport::loadTerrain(const pipeline::TerrainMesh& mesh,
                                  const std::vector<std::string>& texturePaths,
                                  int tileX, int tileY) {
    return terrainRenderer_->loadTerrain(mesh, texturePaths, tileX, tileY);
}

void EditorViewport::clearTerrain() {
    if (terrainRenderer_) terrainRenderer_->clear();
}

void EditorViewport::updateWater(const pipeline::ADTTerrain& terrain, int tileX, int tileY) {
    waterRenderer_.update(terrain, tileX, tileY);
}

void EditorViewport::updateMarkers(const std::vector<PlacedObject>& objects) {
    markerRenderer_.update(objects);
}

void EditorViewport::placeM2(const std::string& path, const glm::vec3& pos,
                              const glm::vec3& rot, float scale) {
    (void)path; (void)pos; (void)rot; (void)scale;
}

void EditorViewport::placeWMO(const std::string& path, const glm::vec3& pos,
                               const glm::vec3& rot) {
    (void)path; (void)pos; (void)rot;
}

void EditorViewport::clearObjects() {
    if (m2Renderer_) {
        vkCtx_->waitAllUploads();
        m2Renderer_->clear();
    }
    if (wmoRenderer_) {
        wmoRenderer_->clearAll();
    }
    markerRenderer_.clear();
}

void EditorViewport::rebuildObjects(const std::vector<PlacedObject>& objects,
                                     const std::vector<CreatureSpawn>& npcs) {
    clearObjects();
    if (objects.empty() && npcs.empty()) return;

    uint32_t nextModelId = 1;
    std::unordered_map<std::string, uint32_t> m2ModelIds, wmoModelIds;

    for (const auto& obj : objects) {
        if (obj.type == PlaceableType::M2 && m2Renderer_) {
            uint32_t modelId;
            auto it = m2ModelIds.find(obj.path);
            if (it != m2ModelIds.end()) {
                modelId = it->second;
            } else {
                auto data = assetManager_->readFile(obj.path);
                if (data.empty()) {
                    LOG_WARNING("M2 file not found in manifest: ", obj.path);
                    continue;
                }
                auto model = pipeline::M2Loader::load(data);

                // WotLK M2s need a separate .skin file for geometry
                if (!model.isValid()) {
                    std::string skinPath = obj.path;
                    auto dotPos = skinPath.rfind('.');
                    if (dotPos != std::string::npos)
                        skinPath = skinPath.substr(0, dotPos) + "00.skin";
                    auto skinData = assetManager_->readFile(skinPath);
                    if (!skinData.empty())
                        pipeline::M2Loader::loadSkin(skinData, model);
                }

                if (!model.isValid()) {
                    LOG_WARNING("M2 failed to parse (", data.size(), " bytes): ", obj.path);
                    continue;
                }

                // Ensure boundRadius is reasonable for culling
                if (model.boundRadius < 1.0f) model.boundRadius = 50.0f;

                modelId = nextModelId++;
                if (!m2Renderer_->loadModel(model, modelId)) {
                    LOG_WARNING("M2 failed to upload to GPU: ", obj.path);
                    continue;
                }
                // Wait for async texture uploads to complete before rendering
                vkCtx_->waitAllUploads();
                vkCtx_->pollUploadBatches();
                LOG_INFO("M2 loaded: ", obj.path, " (modelId=", modelId, ", ",
                         model.vertices.size(), " verts)");
                m2ModelIds[obj.path] = modelId;
            }
            glm::vec3 rotRad = glm::radians(obj.rotation);
            m2Renderer_->createInstance(modelId, obj.position, rotRad, obj.scale);

        } else if (obj.type == PlaceableType::WMO && wmoRenderer_) {
            uint32_t modelId;
            auto it = wmoModelIds.find(obj.path);
            if (it != wmoModelIds.end()) {
                modelId = it->second;
            } else {
                auto data = assetManager_->readFile(obj.path);
                if (data.empty()) {
                    LOG_WARNING("WMO file not found in manifest: ", obj.path);
                    continue;
                }
                auto model = pipeline::WMOLoader::load(data);

                // Load WMO group files (_000.wmo, _001.wmo, etc.)
                std::string basePath = obj.path;
                auto dotPos = basePath.rfind('.');
                if (dotPos != std::string::npos) basePath = basePath.substr(0, dotPos);
                for (uint32_t gi = 0; gi < model.nGroups; gi++) {
                    char groupSuffix[16];
                    std::snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.wmo", gi);
                    std::string groupPath = basePath + groupSuffix;
                    auto groupData = assetManager_->readFile(groupPath);
                    if (!groupData.empty()) {
                        pipeline::WMOLoader::loadGroup(groupData, model, gi);
                    }
                }

                if (!model.isValid()) {
                    LOG_WARNING("WMO failed to parse (", data.size(), " bytes, ",
                                model.nGroups, " groups expected): ", obj.path);
                    continue;
                }

                modelId = nextModelId++;
                if (!wmoRenderer_->loadModel(model, modelId)) {
                    LOG_WARNING("WMO failed to upload to GPU: ", obj.path);
                    continue;
                }
                vkCtx_->waitAllUploads();
                vkCtx_->pollUploadBatches();
                LOG_INFO("WMO loaded: ", obj.path, " (modelId=", modelId, ", ",
                         model.groups.size(), " groups)");
                wmoModelIds[obj.path] = modelId;
            }
            glm::vec3 wmoRotRad = glm::radians(obj.rotation);
            wmoRenderer_->createInstance(modelId, obj.position, wmoRotRad);
        }
    }

    // Render NPC creatures as M2 instances
    if (m2Renderer_) {
        for (const auto& npc : npcs) {
            if (npc.modelPath.empty()) continue;
            uint32_t modelId;
            auto it = m2ModelIds.find(npc.modelPath);
            if (it != m2ModelIds.end()) {
                modelId = it->second;
            } else {
                auto data = assetManager_->readFile(npc.modelPath);
                if (data.empty()) continue;
                auto model = pipeline::M2Loader::load(data);
                if (!model.isValid()) {
                    std::string skinPath = npc.modelPath;
                    auto dotPos = skinPath.rfind('.');
                    if (dotPos != std::string::npos)
                        skinPath = skinPath.substr(0, dotPos) + "00.skin";
                    auto skinData = assetManager_->readFile(skinPath);
                    if (!skinData.empty())
                        pipeline::M2Loader::loadSkin(skinData, model);
                }
                if (!model.isValid()) continue;
                if (model.boundRadius < 1.0f) model.boundRadius = 50.0f;
                modelId = nextModelId++;
                if (!m2Renderer_->loadModel(model, modelId)) continue;
                vkCtx_->waitAllUploads();
                vkCtx_->pollUploadBatches();
                m2ModelIds[npc.modelPath] = modelId;
            }
            glm::vec3 rotRad = glm::radians(glm::vec3(0, 0, npc.orientation));
            m2Renderer_->createInstance(modelId, npc.position, rotRad, 1.0f);
        }
    }

    vkCtx_->waitAllUploads();
    vkCtx_->pollUploadBatches();
}

void EditorViewport::update(float deltaTime) {
    if (m2Renderer_)
        m2Renderer_->update(deltaTime, camera_->getPosition(), camera_->getViewProjectionMatrix());
}

void EditorViewport::setGhostPreview(const std::string& path, const glm::vec3& pos,
                                      const glm::vec3& rotDeg, float scale) {
    if (!m2Renderer_) return;

    // Load model if path changed
    if (path != ghostModelPath_ || ghostModelId_ == 0) {
        clearGhostPreview();
        auto data = assetManager_->readFile(path);
        if (data.empty()) return;
        auto model = pipeline::M2Loader::load(data);
        if (!model.isValid()) {
            std::string skinPath = path;
            auto dotPos = skinPath.rfind('.');
            if (dotPos != std::string::npos)
                skinPath = skinPath.substr(0, dotPos) + "00.skin";
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty())
                pipeline::M2Loader::loadSkin(skinData, model);
        }
        if (!model.isValid()) return;
        if (model.boundRadius < 1.0f) model.boundRadius = 50.0f;

        ghostModelId_ = 60000; // Use a high ID to avoid collision with placed objects
        m2Renderer_->loadModel(model, ghostModelId_);
        vkCtx_->waitAllUploads();
        vkCtx_->pollUploadBatches();
        ghostModelPath_ = path;
    }

    // Create or update ghost instance
    glm::vec3 rotRad = glm::radians(rotDeg);
    if (!ghostActive_) {
        ghostInstanceId_ = m2Renderer_->createInstance(ghostModelId_, pos, rotRad, scale);
        ghostActive_ = (ghostInstanceId_ != 0);
    } else {
        m2Renderer_->setInstancePosition(ghostInstanceId_, pos);
        // Rebuild transform with new rotation/scale
        glm::mat4 mat = glm::mat4(1.0f);
        mat = glm::translate(mat, pos);
        mat = glm::rotate(mat, rotRad.x, glm::vec3(1, 0, 0));
        mat = glm::rotate(mat, rotRad.y, glm::vec3(0, 1, 0));
        mat = glm::rotate(mat, rotRad.z, glm::vec3(0, 0, 1));
        mat = glm::scale(mat, glm::vec3(scale));
        m2Renderer_->setInstanceTransform(ghostInstanceId_, mat);
    }
}

void EditorViewport::clearGhostPreview() {
    if (ghostActive_ && m2Renderer_) {
        m2Renderer_->removeInstance(ghostInstanceId_);
        ghostActive_ = false;
        ghostInstanceId_ = 0;
    }
    if (ghostModelId_ != 0 && m2Renderer_) {
        // Don't unload the model — it might be used by placed objects too
        ghostModelId_ = 0;
        ghostModelPath_.clear();
    }
}

void EditorViewport::render(VkCommandBuffer cmd) {
    updatePerFrameUBO();

    uint32_t frame = vkCtx_->getCurrentFrame();
    VkDescriptorSet perFrameSet = perFrameDescSets_[frame];

    terrainRenderer_->render(cmd, perFrameSet, *camera_);

    if (m2Renderer_)
        m2Renderer_->render(cmd, perFrameSet, *camera_);
    if (wmoRenderer_)
        wmoRenderer_->render(cmd, perFrameSet, *camera_);

    waterRenderer_.render(cmd, perFrameSet);
    gizmo_.render(cmd, perFrameSet);
}

void EditorViewport::setWireframe(bool enabled) {
    wireframe_ = enabled;
    if (terrainRenderer_) terrainRenderer_->setWireframe(enabled);
}

bool EditorViewport::createPerFrameResources() {
    VkDevice device = vkCtx_->getDevice();

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &perFrameSetLayout_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &sceneDescPool_) != VK_SUCCESS)
        return false;

    dummyShadowTexture_ = std::make_unique<rendering::VkTexture>();
    if (!dummyShadowTexture_->createDepth(*vkCtx_, 1, 1)) return false;

    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampCI.compareEnable = VK_TRUE;
    sampCI.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowSampler_ = vkCtx_->getOrCreateSampler(sampCI);

    vkCtx_->immediateSubmit([this](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = dummyShadowTexture_->getImage();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    });

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(rendering::GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(vkCtx_->getAllocator(), &bufInfo, &allocInfo,
                &perFrameUBOs_[i], &perFrameUBOAllocs_[i], &mapInfo) != VK_SUCCESS)
            return false;
        perFrameUBOMapped_[i] = mapInfo.pMappedData;

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescPool_;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameSetLayout_;
        if (vkAllocateDescriptorSets(device, &setAlloc, &perFrameDescSets_[i]) != VK_SUCCESS)
            return false;

        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = perFrameUBOs_[i];
        descBuf.offset = 0;
        descBuf.range = sizeof(rendering::GPUPerFrameData);

        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler = shadowSampler_;
        shadowImgInfo.imageView = dummyShadowTexture_->getImageView();
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = perFrameDescSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = perFrameDescSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImgInfo;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    return true;
}

void EditorViewport::destroyPerFrameResources() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (perFrameUBOs_[i]) {
            vmaDestroyBuffer(vkCtx_->getAllocator(), perFrameUBOs_[i], perFrameUBOAllocs_[i]);
            perFrameUBOs_[i] = VK_NULL_HANDLE;
        }
    }
    if (dummyShadowTexture_) {
        dummyShadowTexture_->destroy(device, vkCtx_->getAllocator());
        dummyShadowTexture_.reset();
    }
    if (sceneDescPool_) {
        vkDestroyDescriptorPool(device, sceneDescPool_, nullptr);
        sceneDescPool_ = VK_NULL_HANDLE;
    }
    if (perFrameSetLayout_) {
        vkDestroyDescriptorSetLayout(device, perFrameSetLayout_, nullptr);
        perFrameSetLayout_ = VK_NULL_HANDLE;
    }
}

void EditorViewport::updatePerFrameUBO() {
    uint32_t frame = vkCtx_->getCurrentFrame();

    rendering::GPUPerFrameData data{};
    data.view = camera_->getViewMatrix();
    data.projection = camera_->getProjectionMatrix();
    data.lightSpaceMatrix = glm::mat4(1.0f);
    data.lightDir = glm::vec4(glm::normalize(glm::vec3(0.5f, -1.0f, 0.3f)), 0.0f);
    data.lightColor = glm::vec4(1.0f, 0.95f, 0.85f, 0.0f);
    data.ambientColor = glm::vec4(0.3f, 0.3f, 0.35f, 0.0f);
    data.viewPos = glm::vec4(camera_->getPosition(), 0.0f);
    data.fogColor = glm::vec4(0.6f, 0.7f, 0.8f, 0.0f);
    data.fogParams = glm::vec4(5000.0f, 10000.0f, 0.0f, 0.0f);
    data.shadowParams = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);

    std::memcpy(perFrameUBOMapped_[frame], &data, sizeof(data));
}

} // namespace editor
} // namespace wowee
