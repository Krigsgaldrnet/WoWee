#include "ui/chat/chat_settings.hpp"
#include <imgui.h>

namespace wowee { namespace ui {

// Reset all chat settings to defaults.
void ChatSettings::restoreDefaults() {
    showTimestamps       = false;
    fontSize             = 1;
    autoJoinGeneral      = true;
    autoJoinTrade        = true;
    autoJoinLocalDefense = true;
    autoJoinLFG          = true;
    autoJoinLocal        = true;
}

// Render the "Chat" tab inside the Settings window.
void ChatSettings::renderSettingsTab(std::function<void()> saveSettingsFn) {
    ImGui::Spacing();

    ImGui::Text("Appearance");
    ImGui::Separator();

    if (ImGui::Checkbox("Show Timestamps", &showTimestamps)) {
        saveSettingsFn();
    }
    ImGui::SetItemTooltip("Show [HH:MM] before each chat message");

    const char* fontSizes[] = { "Small", "Medium", "Large" };
    if (ImGui::Combo("Chat Font Size", &fontSize, fontSizes, 3)) {
        saveSettingsFn();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Text("Auto-Join Channels");
    ImGui::Separator();

    if (ImGui::Checkbox("General", &autoJoinGeneral)) saveSettingsFn();
    if (ImGui::Checkbox("Trade", &autoJoinTrade)) saveSettingsFn();
    if (ImGui::Checkbox("LocalDefense", &autoJoinLocalDefense)) saveSettingsFn();
    if (ImGui::Checkbox("LookingForGroup", &autoJoinLFG)) saveSettingsFn();
    if (ImGui::Checkbox("Local", &autoJoinLocal)) saveSettingsFn();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Text("Joined Channels");
    ImGui::Separator();

    ImGui::TextDisabled("Use /join and /leave commands in chat to manage channels.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Restore Chat Defaults", ImVec2(-1, 0))) {
        restoreDefaults();
        saveSettingsFn();
    }
}

} // namespace ui
} // namespace wowee
