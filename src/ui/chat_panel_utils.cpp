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

// getChatTypeName / getChatTypeColor moved to ChatTabManager (Phase 1.3)


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

// renderBubbles delegates to ChatBubbleManager (Phase 1.4)
void ChatPanel::renderBubbles(game::GameHandler& gameHandler) {
    bubbleManager_.render(gameHandler, services_);
}


// ---- Public interface methods ----

// setupCallbacks delegates to ChatBubbleManager (Phase 1.4)
void ChatPanel::setupCallbacks(game::GameHandler& gameHandler) {
    bubbleManager_.setupCallback(gameHandler);
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
    if (chatInputCooldown_ > 0) return;  // suppress re-activation right after send
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

} // namespace ui
} // namespace wowee
