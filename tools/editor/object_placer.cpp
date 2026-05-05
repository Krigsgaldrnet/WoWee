#include "object_placer.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <random>

namespace wowee {
namespace editor {

void ObjectPlacer::setActivePath(const std::string& path, PlaceableType type) {
    activePath_ = path;
    activeType_ = type;
}

uint32_t ObjectPlacer::nextUniqueId() {
    return uniqueIdCounter_++;
}

void ObjectPlacer::placeObject(const glm::vec3& position) {
    if (activePath_.empty()) return;

    PlacedObject obj;
    obj.type = activeType_;
    obj.path = activePath_;
    obj.nameId = 0;
    obj.uniqueId = nextUniqueId();
    obj.position = position;
    float rotY = placementRotY_;
    if (randomRotation_) {
        static std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(0.0f, 360.0f);
        rotY = dist(rng);
    }
    obj.rotation = glm::vec3(0.0f, rotY, 0.0f);
    obj.scale = placementScale_;
    obj.selected = false;

    objects_.push_back(obj);
    undoStack_.push_back(static_cast<int>(objects_.size() - 1));
    if (undoStack_.size() > 50) undoStack_.erase(undoStack_.begin());
    LOG_INFO("Placed ", (activeType_ == PlaceableType::M2 ? "M2" : "WMO"),
             ": ", activePath_, " at (", position.x, ",", position.y, ",", position.z, ")");
}

int ObjectPlacer::selectAt(const rendering::Ray& ray, float maxDist) {
    clearSelection();

    float bestDist = maxDist;
    int bestIdx = -1;

    for (int i = 0; i < static_cast<int>(objects_.size()); i++) {
        // Simple sphere test (radius based on scale)
        float radius = 5.0f * objects_[i].scale;
        glm::vec3 oc = ray.origin - objects_[i].position;
        float b = glm::dot(oc, ray.direction);
        float c = glm::dot(oc, oc) - radius * radius;
        float disc = b * b - c;
        if (disc < 0) continue;

        float t = -b - std::sqrt(disc);
        if (t < 0) t = -b + std::sqrt(disc);
        if (t > 0 && t < bestDist) {
            bestDist = t;
            bestIdx = i;
        }
    }

    if (bestIdx >= 0) {
        selectedIdx_ = bestIdx;
        objects_[bestIdx].selected = true;
    }
    return bestIdx;
}

void ObjectPlacer::clearSelection() {
    if (selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(objects_.size()))
        objects_[selectedIdx_].selected = false;
    selectedIdx_ = -1;
}

PlacedObject* ObjectPlacer::getSelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(objects_.size())) return nullptr;
    return &objects_[selectedIdx_];
}

void ObjectPlacer::moveSelected(const glm::vec3& delta) {
    if (auto* obj = getSelected()) obj->position += delta;
}

void ObjectPlacer::rotateSelected(const glm::vec3& deltaDeg) {
    if (auto* obj = getSelected()) obj->rotation += deltaDeg;
}

void ObjectPlacer::scaleSelected(float delta) {
    if (auto* obj = getSelected())
        obj->scale = std::max(0.1f, obj->scale + delta);
}

void ObjectPlacer::deleteSelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(objects_.size())) return;
    objects_.erase(objects_.begin() + selectedIdx_);
    selectedIdx_ = -1;
}

void ObjectPlacer::scatter(const glm::vec3& center, float radius, int count,
                            float minScale, float maxScale) {
    if (activePath_.empty()) return;
    std::mt19937 rng(static_cast<uint32_t>(center.x * 100 + center.y * 37 + objects_.size()));
    std::uniform_real_distribution<float> distAngle(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> distDist(0.0f, 1.0f);
    std::uniform_real_distribution<float> distRot(0.0f, 360.0f);
    std::uniform_real_distribution<float> distScale(minScale, maxScale);

    for (int i = 0; i < count; i++) {
        float angle = distAngle(rng);
        float dist = std::sqrt(distDist(rng)) * radius;
        glm::vec3 pos = center + glm::vec3(std::cos(angle) * dist, std::sin(angle) * dist, 0.0f);

        PlacedObject obj;
        obj.type = activeType_;
        obj.path = activePath_;
        obj.nameId = 0;
        obj.uniqueId = nextUniqueId();
        obj.position = pos;
        obj.rotation = glm::vec3(0.0f, distRot(rng), 0.0f);
        obj.scale = distScale(rng);
        obj.selected = false;
        objects_.push_back(obj);
    }
    LOG_INFO("Scattered ", count, " objects in radius ", radius);
}

void ObjectPlacer::undoLastPlace() {
    if (undoStack_.empty()) return;
    int idx = undoStack_.back();
    undoStack_.pop_back();
    if (idx >= 0 && idx < static_cast<int>(objects_.size())) {
        if (selectedIdx_ == idx) selectedIdx_ = -1;
        else if (selectedIdx_ > idx) selectedIdx_--;
        objects_.erase(objects_.begin() + idx);
        // Adjust remaining undo indices
        for (auto& i : undoStack_) { if (i > idx) i--; }
    }
}

void ObjectPlacer::syncToTerrain() {
    if (!terrain_) return;

    terrain_->doodadNames.clear();
    terrain_->doodadPlacements.clear();
    terrain_->wmoNames.clear();
    terrain_->wmoPlacements.clear();

    // Build name lists and placements
    std::vector<std::string> m2Names, wmoNames;

    for (const auto& obj : objects_) {
        if (obj.type == PlaceableType::M2) {
            // Find or add name
            uint32_t nameId = 0;
            for (uint32_t i = 0; i < m2Names.size(); i++) {
                if (m2Names[i] == obj.path) { nameId = i; goto foundM2; }
            }
            nameId = static_cast<uint32_t>(m2Names.size());
            m2Names.push_back(obj.path);
            foundM2:

            pipeline::ADTTerrain::DoodadPlacement dp{};
            dp.nameId = nameId;
            dp.uniqueId = obj.uniqueId;
            dp.position[0] = obj.position.x;
            dp.position[1] = obj.position.y;
            dp.position[2] = obj.position.z;
            dp.rotation[0] = obj.rotation.x;
            dp.rotation[1] = obj.rotation.y;
            dp.rotation[2] = obj.rotation.z;
            dp.scale = static_cast<uint16_t>(obj.scale * 1024.0f);
            dp.flags = 0;
            terrain_->doodadPlacements.push_back(dp);

        } else {
            uint32_t nameId = 0;
            for (uint32_t i = 0; i < wmoNames.size(); i++) {
                if (wmoNames[i] == obj.path) { nameId = i; goto foundWMO; }
            }
            nameId = static_cast<uint32_t>(wmoNames.size());
            wmoNames.push_back(obj.path);
            foundWMO:

            pipeline::ADTTerrain::WMOPlacement wp{};
            wp.nameId = nameId;
            wp.uniqueId = obj.uniqueId;
            wp.position[0] = obj.position.x;
            wp.position[1] = obj.position.y;
            wp.position[2] = obj.position.z;
            wp.rotation[0] = obj.rotation.x;
            wp.rotation[1] = obj.rotation.y;
            wp.rotation[2] = obj.rotation.z;
            wp.flags = 0;
            wp.doodadSet = 0;
            terrain_->wmoPlacements.push_back(wp);
        }
    }

    terrain_->doodadNames = std::move(m2Names);
    terrain_->wmoNames = std::move(wmoNames);
}

} // namespace editor
} // namespace wowee
