#include "editor_camera.hpp"
#include <glm/gtc/constants.hpp>
#include <algorithm>

namespace wowee {
namespace editor {

EditorCamera::EditorCamera() {
    camera_.setPosition(glm::vec3(0.0f, 0.0f, 200.0f));
    camera_.setFov(60.0f);
    camera_.setRotation(0.0f, -30.0f);
    yaw_ = 0.0f;
    pitch_ = -30.0f;
}

void EditorCamera::update(float deltaTime) {
    float moveSpeed = speed_ * deltaTime;
    if (keyShift_) moveSpeed *= 3.0f;

    glm::vec3 forward = camera_.getForward();
    glm::vec3 right = camera_.getRight();
    glm::vec3 up(0.0f, 0.0f, 1.0f); // Z-up (WoW render coords)

    glm::vec3 pos = camera_.getPosition();
    if (keyW_) pos += forward * moveSpeed;
    if (keyS_) pos -= forward * moveSpeed;
    if (keyD_) pos += right * moveSpeed;
    if (keyA_) pos -= right * moveSpeed;
    if (keyE_) pos += up * moveSpeed;
    if (keyQ_) pos -= up * moveSpeed;

    camera_.setPosition(pos);
}

void EditorCamera::processMouseMotion(int dx, int dy) {
    if (!rightMouseDown_) return;

    constexpr float sensitivity = 0.15f; // degrees per pixel
    yaw_ += static_cast<float>(dx) * sensitivity;
    pitch_ -= static_cast<float>(dy) * sensitivity;
    pitch_ = std::clamp(pitch_, -89.0f, 89.0f);

    camera_.setRotation(yaw_, pitch_);
}

void EditorCamera::processMouseWheel(float delta, bool shiftHeld) {
    if (shiftHeld) {
        speed_ = std::clamp(speed_ + delta * 20.0f, 10.0f, 2000.0f);
    } else {
        glm::vec3 pos = camera_.getPosition();
        pos += camera_.getForward() * delta * speed_ * 0.3f;
        camera_.setPosition(pos);
    }
}

void EditorCamera::processKeyEvent(const SDL_KeyboardEvent& event) {
    bool pressed = (event.type == SDL_KEYDOWN);
    switch (event.keysym.scancode) {
        case SDL_SCANCODE_W: keyW_ = pressed; break;
        case SDL_SCANCODE_A: keyA_ = pressed; break;
        case SDL_SCANCODE_S: keyS_ = pressed; break;
        case SDL_SCANCODE_D: keyD_ = pressed; break;
        case SDL_SCANCODE_Q: keyQ_ = pressed; break;
        case SDL_SCANCODE_E: keyE_ = pressed; break;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: keyShift_ = pressed; break;
        default: break;
    }
}

void EditorCamera::processMouseButton(const SDL_MouseButtonEvent& event) {
    if (event.button == SDL_BUTTON_RIGHT)
        rightMouseDown_ = (event.type == SDL_MOUSEBUTTONDOWN);
}

void EditorCamera::setPosition(const glm::vec3& pos) {
    camera_.setPosition(pos);
}

void EditorCamera::setYawPitch(float yaw, float pitch) {
    yaw_ = yaw;
    pitch_ = pitch;
    camera_.setRotation(yaw_, pitch_);
}

} // namespace editor
} // namespace wowee
