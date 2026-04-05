#include "ui/chat_panel.hpp"
#include "ui/ui_colors.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include "core/coordinates.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cstring>
#include <unordered_map>

namespace {
    using namespace wowee::ui::colors;
    constexpr auto& kColorRed        = kRed;
    constexpr auto& kColorBrightGreen= kBrightGreen;
    constexpr auto& kColorYellow     = kYellow;
} // namespace

namespace wowee { namespace ui {

const char* ChatPanel::getChatTypeName(game::ChatType type) const {
    switch (type) {
        case game::ChatType::SAY: return "Say";
        case game::ChatType::YELL: return "Yell";
        case game::ChatType::EMOTE: return "Emote";
        case game::ChatType::TEXT_EMOTE: return "Emote";
        case game::ChatType::PARTY: return "Party";
        case game::ChatType::GUILD: return "Guild";
        case game::ChatType::OFFICER: return "Officer";
        case game::ChatType::RAID: return "Raid";
        case game::ChatType::RAID_LEADER: return "Raid Leader";
        case game::ChatType::RAID_WARNING: return "Raid Warning";
        case game::ChatType::BATTLEGROUND: return "Battleground";
        case game::ChatType::BATTLEGROUND_LEADER: return "Battleground Leader";
        case game::ChatType::WHISPER: return "Whisper";
        case game::ChatType::WHISPER_INFORM: return "To";
        case game::ChatType::SYSTEM: return "System";
        case game::ChatType::MONSTER_SAY: return "Say";
        case game::ChatType::MONSTER_YELL: return "Yell";
        case game::ChatType::MONSTER_EMOTE: return "Emote";
        case game::ChatType::CHANNEL: return "Channel";
        case game::ChatType::ACHIEVEMENT: return "Achievement";
        case game::ChatType::DND: return "DND";
        case game::ChatType::AFK: return "AFK";
        case game::ChatType::BG_SYSTEM_NEUTRAL:
        case game::ChatType::BG_SYSTEM_ALLIANCE:
        case game::ChatType::BG_SYSTEM_HORDE: return "System";
        default: return "Unknown";
    }
}


ImVec4 ChatPanel::getChatTypeColor(game::ChatType type) const {
    switch (type) {
        case game::ChatType::SAY:
            return ui::colors::kWhite;  // White
        case game::ChatType::YELL:
            return kColorRed;  // Red
        case game::ChatType::EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange
        case game::ChatType::TEXT_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange
        case game::ChatType::PARTY:
            return ImVec4(0.5f, 0.5f, 1.0f, 1.0f);  // Light blue
        case game::ChatType::GUILD:
            return kColorBrightGreen;  // Green
        case game::ChatType::OFFICER:
            return ImVec4(0.3f, 0.8f, 0.3f, 1.0f);  // Dark green
        case game::ChatType::RAID:
            return ImVec4(1.0f, 0.5f, 0.0f, 1.0f);  // Orange
        case game::ChatType::RAID_LEADER:
            return ImVec4(1.0f, 0.4f, 0.0f, 1.0f);  // Darker orange
        case game::ChatType::RAID_WARNING:
            return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // Red
        case game::ChatType::BATTLEGROUND:
            return ImVec4(1.0f, 0.6f, 0.0f, 1.0f);  // Orange-gold
        case game::ChatType::BATTLEGROUND_LEADER:
            return ImVec4(1.0f, 0.5f, 0.0f, 1.0f);  // Orange
        case game::ChatType::WHISPER:
            return ImVec4(1.0f, 0.5f, 1.0f, 1.0f);  // Pink
        case game::ChatType::WHISPER_INFORM:
            return ImVec4(1.0f, 0.5f, 1.0f, 1.0f);  // Pink
        case game::ChatType::SYSTEM:
            return kColorYellow;  // Yellow
        case game::ChatType::MONSTER_SAY:
            return ui::colors::kWhite;  // White (same as SAY)
        case game::ChatType::MONSTER_YELL:
            return kColorRed;  // Red (same as YELL)
        case game::ChatType::MONSTER_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange (same as EMOTE)
        case game::ChatType::CHANNEL:
            return ImVec4(1.0f, 0.7f, 0.7f, 1.0f);  // Light pink
        case game::ChatType::ACHIEVEMENT:
            return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // Bright yellow
        case game::ChatType::GUILD_ACHIEVEMENT:
            return colors::kWarmGold; // Gold
        case game::ChatType::SKILL:
            return colors::kCyan;  // Cyan
        case game::ChatType::LOOT:
            return ImVec4(0.8f, 0.5f, 1.0f, 1.0f);  // Light purple
        case game::ChatType::MONSTER_WHISPER:
        case game::ChatType::RAID_BOSS_WHISPER:
            return ImVec4(1.0f, 0.5f, 1.0f, 1.0f);  // Pink (same as WHISPER)
        case game::ChatType::RAID_BOSS_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange (same as EMOTE)
        case game::ChatType::MONSTER_PARTY:
            return ImVec4(0.5f, 0.5f, 1.0f, 1.0f);  // Light blue (same as PARTY)
        case game::ChatType::BG_SYSTEM_NEUTRAL:
            return colors::kWarmGold; // Gold
        case game::ChatType::BG_SYSTEM_ALLIANCE:
            return ImVec4(0.3f, 0.6f, 1.0f, 1.0f);  // Blue
        case game::ChatType::BG_SYSTEM_HORDE:
            return kColorRed;  // Red
        case game::ChatType::AFK:
        case game::ChatType::DND:
            return ImVec4(0.85f, 0.85f, 0.85f, 0.8f); // Light gray
        default:
            return ui::colors::kLightGray;  // Gray
    }
}


std::string ChatPanel::replaceGenderPlaceholders(const std::string& text, game::GameHandler& gameHandler) {
    // Get player gender, pronouns, and name
    game::Gender gender = game::Gender::NONBINARY;
    std::string playerName = "Adventurer";
    const auto* character = gameHandler.getActiveCharacter();
    if (character) {
        gender = character->gender;
        if (!character->name.empty()) {
            playerName = character->name;
        }
    }
    game::Pronouns pronouns = game::Pronouns::forGender(gender);

    std::string result = text;

    // Helper to trim whitespace
    auto trim = [](std::string& s) {
        const char* ws = " \t\n\r";
        size_t start = s.find_first_not_of(ws);
        if (start == std::string::npos) { s.clear(); return; }
        size_t end = s.find_last_not_of(ws);
        s = s.substr(start, end - start + 1);
    };

    // Replace $g/$G placeholders first.
    size_t pos = 0;
    while ((pos = result.find('$', pos)) != std::string::npos) {
        if (pos + 1 >= result.length()) break;
        char marker = result[pos + 1];
        if (marker != 'g' && marker != 'G') { pos++; continue; }

        size_t endPos = result.find(';', pos);
        if (endPos == std::string::npos) { pos += 2; continue; }

        std::string placeholder = result.substr(pos + 2, endPos - pos - 2);

        // Split by colons
        std::vector<std::string> parts;
        size_t start = 0;
        size_t colonPos;
        while ((colonPos = placeholder.find(':', start)) != std::string::npos) {
            std::string part = placeholder.substr(start, colonPos - start);
            trim(part);
            parts.push_back(part);
            start = colonPos + 1;
        }
        // Add the last part
        std::string lastPart = placeholder.substr(start);
        trim(lastPart);
        parts.push_back(lastPart);

        // Select appropriate text based on gender
        std::string replacement;
        if (parts.size() >= 3) {
            // Three options: male, female, nonbinary
            switch (gender) {
                case game::Gender::MALE:
                    replacement = parts[0];
                    break;
                case game::Gender::FEMALE:
                    replacement = parts[1];
                    break;
                case game::Gender::NONBINARY:
                    replacement = parts[2];
                    break;
            }
        } else if (parts.size() >= 2) {
            // Two options: male, female (use first for nonbinary)
            switch (gender) {
                case game::Gender::MALE:
                    replacement = parts[0];
                    break;
                case game::Gender::FEMALE:
                    replacement = parts[1];
                    break;
                case game::Gender::NONBINARY:
                    // Default to gender-neutral: use the shorter/simpler option
                    replacement = parts[0].length() <= parts[1].length() ? parts[0] : parts[1];
                    break;
            }
        } else {
            // Malformed placeholder
            pos = endPos + 1;
            continue;
        }

        result.replace(pos, endPos - pos + 1, replacement);
        pos += replacement.length();
    }

    // Resolve class and race names for $C and $R placeholders
    std::string className = "Adventurer";
    std::string raceName = "Unknown";
    if (character) {
        className = game::getClassName(character->characterClass);
        raceName = game::getRaceName(character->race);
    }

    // Replace simple placeholders.
    // $n/$N = player name, $c/$C = class name, $r/$R = race name
    // $p = subject pronoun (he/she/they)
    // $o = object pronoun (him/her/them)
    // $s = possessive adjective (his/her/their)
    // $S = possessive pronoun (his/hers/theirs)
    // $b/$B = line break
    pos = 0;
    while ((pos = result.find('$', pos)) != std::string::npos) {
        if (pos + 1 >= result.length()) break;

        char code = result[pos + 1];
        std::string replacement;
        switch (code) {
            case 'n': case 'N': replacement = playerName; break;
            case 'c': case 'C': replacement = className; break;
            case 'r': case 'R': replacement = raceName; break;
            case 'p': replacement = pronouns.subject; break;
            case 'o': replacement = pronouns.object; break;
            case 's': replacement = pronouns.possessive; break;
            case 'S': replacement = pronouns.possessiveP; break;
            case 'b': case 'B': replacement = "\n"; break;
            case 'g': case 'G': pos++; continue;
            default: pos++; continue;
        }

        result.replace(pos, 2, replacement);
        pos += replacement.length();
    }

    // WoW markup linebreak token.
    pos = 0;
    while ((pos = result.find("|n", pos)) != std::string::npos) {
        result.replace(pos, 2, "\n");
        pos += 1;
    }
    pos = 0;
    while ((pos = result.find("|N", pos)) != std::string::npos) {
        result.replace(pos, 2, "\n");
        pos += 1;
    }

    return result;
}

void ChatPanel::renderBubbles(game::GameHandler& gameHandler) {
    if (chatBubbles_.empty()) return;

    auto* renderer = services_.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!camera) return;

    auto* window = services_.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    // Get delta time from ImGui
    float dt = ImGui::GetIO().DeltaTime;

    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    // Update and render bubbles
    for (int i = static_cast<int>(chatBubbles_.size()) - 1; i >= 0; --i) {
        auto& bubble = chatBubbles_[i];
        bubble.timeRemaining -= dt;
        if (bubble.timeRemaining <= 0.0f) {
            chatBubbles_.erase(chatBubbles_.begin() + i);
            continue;
        }

        // Get entity position
        auto entity = gameHandler.getEntityManager().getEntity(bubble.senderGuid);
        if (!entity) continue;

        // Convert canonical → render coordinates, offset up by 2.5 units for bubble above head
        glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ() + 2.5f);
        glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

        // Project to screen
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.0f) continue;  // Behind camera

        glm::vec2 ndc(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
        float screenX = (ndc.x * 0.5f + 0.5f) * screenW;
        // Camera bakes the Vulkan Y-flip into the projection matrix:
        // NDC y=-1 is top, y=1 is bottom — same convention as nameplate/minimap projection.
        float screenY = (ndc.y * 0.5f + 0.5f) * screenH;

        // Skip if off-screen
        if (screenX < -200.0f || screenX > screenW + 200.0f ||
            screenY < -100.0f || screenY > screenH + 100.0f) continue;

        // Fade alpha over last 2 seconds
        float alpha = 1.0f;
        if (bubble.timeRemaining < 2.0f) {
            alpha = bubble.timeRemaining / 2.0f;
        }

        // Draw bubble window
        std::string winId = "##ChatBubble" + std::to_string(bubble.senderGuid);
        ImGui::SetNextWindowPos(ImVec2(screenX, screenY), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.7f * alpha);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

        ImGui::Begin(winId.c_str(), nullptr, flags);

        ImVec4 textColor = bubble.isYell
            ? ImVec4(1.0f, 0.2f, 0.2f, alpha)
            : ImVec4(1.0f, 1.0f, 1.0f, alpha);

        ImGui::PushStyleColor(ImGuiCol_Text, textColor);
        ImGui::PushTextWrapPos(200.0f);
        ImGui::TextWrapped("%s", bubble.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}


// ---- Public interface methods ----

void ChatPanel::setupCallbacks(game::GameHandler& gameHandler) {
    if (!chatBubbleCallbackSet_) {
        gameHandler.setChatBubbleCallback([this](uint64_t guid, const std::string& msg, bool isYell) {
            float duration = 8.0f + static_cast<float>(msg.size()) * 0.06f;
            if (isYell) duration += 2.0f;
            if (duration > 15.0f) duration = 15.0f;

            // Replace existing bubble for same sender
            for (auto& b : chatBubbles_) {
                if (b.senderGuid == guid) {
                    b.message = msg;
                    b.timeRemaining = duration;
                    b.totalDuration = duration;
                    b.isYell = isYell;
                    return;
                }
            }
            // Evict oldest if too many
            if (chatBubbles_.size() >= 10) {
                chatBubbles_.erase(chatBubbles_.begin());
            }
            chatBubbles_.push_back({guid, msg, duration, duration, isYell});
        });
        chatBubbleCallbackSet_ = true;
    }
}

void ChatPanel::insertChatLink(const std::string& link) {
    if (link.empty()) return;
    size_t curLen = strlen(chatInputBuffer_);
    if (curLen + link.size() + 1 < sizeof(chatInputBuffer_)) {
        strncat(chatInputBuffer_, link.c_str(), sizeof(chatInputBuffer_) - curLen - 1);
        chatInputMoveCursorToEnd_ = true;
        refocusChatInput_ = true;
    }
}

void ChatPanel::activateSlashInput() {
    refocusChatInput_ = true;
    chatInputBuffer_[0] = '/';
    chatInputBuffer_[1] = '\0';
    chatInputMoveCursorToEnd_ = true;
}

void ChatPanel::activateInput() {
    refocusChatInput_ = true;
}

void ChatPanel::setWhisperTarget(const std::string& name) {
    selectedChatType_ = 4;  // WHISPER
    strncpy(whisperTargetBuffer_, name.c_str(), sizeof(whisperTargetBuffer_) - 1);
    whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
    refocusChatInput_ = true;
}

ChatPanel::SlashCommands ChatPanel::consumeSlashCommands() {
    SlashCommands result = slashCmds_;
    slashCmds_ = {};
    return result;
}

void ChatPanel::renderSettingsTab(std::function<void()> saveSettingsFn) {
    ImGui::Spacing();

    ImGui::Text("Appearance");
    ImGui::Separator();

    if (ImGui::Checkbox("Show Timestamps", &chatShowTimestamps)) {
        saveSettingsFn();
    }
    ImGui::SetItemTooltip("Show [HH:MM] before each chat message");

    const char* fontSizes[] = { "Small", "Medium", "Large" };
    if (ImGui::Combo("Chat Font Size", &chatFontSize, fontSizes, 3)) {
        saveSettingsFn();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Text("Auto-Join Channels");
    ImGui::Separator();

    if (ImGui::Checkbox("General", &chatAutoJoinGeneral)) saveSettingsFn();
    if (ImGui::Checkbox("Trade", &chatAutoJoinTrade)) saveSettingsFn();
    if (ImGui::Checkbox("LocalDefense", &chatAutoJoinLocalDefense)) saveSettingsFn();
    if (ImGui::Checkbox("LookingForGroup", &chatAutoJoinLFG)) saveSettingsFn();
    if (ImGui::Checkbox("Local", &chatAutoJoinLocal)) saveSettingsFn();

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

void ChatPanel::restoreDefaults() {
    chatShowTimestamps = false;
    chatFontSize = 1;
    chatAutoJoinGeneral = true;
    chatAutoJoinTrade = true;
    chatAutoJoinLocalDefense = true;
    chatAutoJoinLFG = true;
    chatAutoJoinLocal = true;
}

} // namespace ui
} // namespace wowee
