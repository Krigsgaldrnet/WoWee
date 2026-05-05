#pragma once

#include "rendering/vk_frame_data.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/camera.hpp"
#include "editor_water.hpp"
#include "editor_markers.hpp"
#include "transform_gizmo.hpp"
#include "object_placer.hpp"
#include "npc_spawner.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class VkContext; class VkTexture; }

namespace editor {

class EditorViewport {
public:
    EditorViewport();
    ~EditorViewport();

    bool initialize(rendering::VkContext* ctx, pipeline::AssetManager* am, rendering::Camera* cam);
    void shutdown();

    bool loadTerrain(const pipeline::TerrainMesh& mesh,
                     const std::vector<std::string>& texturePaths,
                     int tileX, int tileY);
    void clearTerrain();

    void updateWater(const pipeline::ADTTerrain& terrain, int tileX, int tileY);
    void updateMarkers(const std::vector<PlacedObject>& objects);
    void updateNpcMarkers(const std::vector<CreatureSpawn>& npcs);
    void placeM2(const std::string& path, const glm::vec3& pos, const glm::vec3& rot, float scale);
    void placeWMO(const std::string& path, const glm::vec3& pos, const glm::vec3& rot);
    void clearObjects();
    void rebuildObjects(const std::vector<PlacedObject>& objects,
                        const std::vector<CreatureSpawn>& npcs = {});

    void update(float deltaTime);
    void render(VkCommandBuffer cmd);

    // Ghost preview for placement
    void setGhostPreview(const std::string& path, const glm::vec3& pos,
                         const glm::vec3& rotDeg, float scale);
    void clearGhostPreview();

    TransformGizmo& getGizmo() { return gizmo_; }
    void setBrushIndicator(const glm::vec3& center, float radius, bool active);
    void setPathPreview(const glm::vec3& start, const glm::vec3& end, float width, bool visible);

    void setWireframe(bool enabled);
    bool isWireframe() const { return wireframe_; }

    void setClearColor(float r, float g, float b) { clearR_=r; clearG_=g; clearB_=b; }
    void getClearColor(float& r, float& g, float& b) const { r=clearR_; g=clearG_; b=clearB_; }
    void setLightDir(const glm::vec3& d) { lightDir_ = d; }
    glm::vec3 getLightDir() const { return lightDir_; }

    rendering::TerrainRenderer* getTerrainRenderer() { return terrainRenderer_.get(); }
    rendering::M2Renderer* getM2Renderer() { return m2Renderer_.get(); }

private:
    bool createPerFrameResources();
    void destroyPerFrameResources();
    void updatePerFrameUBO();

    rendering::VkContext* vkCtx_ = nullptr;
    pipeline::AssetManager* assetManager_ = nullptr;
    rendering::Camera* camera_ = nullptr;

    std::unique_ptr<rendering::TerrainRenderer> terrainRenderer_;
    std::unique_ptr<rendering::M2Renderer> m2Renderer_;
    std::unique_ptr<rendering::WMORenderer> wmoRenderer_;
    EditorWater waterRenderer_;
    EditorMarkers markerRenderer_;
    TransformGizmo gizmo_;

    static constexpr uint32_t MAX_FRAMES = 2;
    VkDescriptorSetLayout perFrameSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescPool_ = VK_NULL_HANDLE;
    VkDescriptorSet perFrameDescSets_[MAX_FRAMES] = {};
    VkBuffer perFrameUBOs_[MAX_FRAMES] = {};
    VmaAllocation perFrameUBOAllocs_[MAX_FRAMES] = {};
    void* perFrameUBOMapped_[MAX_FRAMES] = {};

    std::unique_ptr<rendering::VkTexture> dummyShadowTexture_;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;

    bool wireframe_ = false;
    float clearR_ = 0.15f, clearG_ = 0.15f, clearB_ = 0.2f;
    glm::vec3 lightDir_ = glm::normalize(glm::vec3(0.5f, -1.0f, 0.3f));

    // Ghost preview state
    std::string ghostModelPath_;
    uint32_t ghostModelId_ = 0;
    uint32_t ghostInstanceId_ = 0;
    bool ghostActive_ = false;

    // Brush indicator
    VkBuffer brushVB_ = VK_NULL_HANDLE;
    VmaAllocation brushVBAlloc_ = VK_NULL_HANDLE;
    uint32_t brushVertCount_ = 0;
    bool brushVisible_ = false;

    // NPC position markers (always visible fallback)
    VkBuffer npcMarkerVB_ = VK_NULL_HANDLE;
    VmaAllocation npcMarkerVBAlloc_ = VK_NULL_HANDLE;
    uint32_t npcMarkerVertCount_ = 0;

    // Path preview line
    VkBuffer pathVB_ = VK_NULL_HANDLE;
    VmaAllocation pathVBAlloc_ = VK_NULL_HANDLE;
    uint32_t pathVertCount_ = 0;
    bool pathVisible_ = false;
};

} // namespace editor
} // namespace wowee
