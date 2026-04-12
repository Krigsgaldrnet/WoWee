#include "ui/chat_panel.hpp"
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat/game_state_adapter.hpp"
#include "ui/chat/input_modifier_adapter.hpp"
#include "ui/chat/chat_utils.hpp"
#include "ui/chat/gm_command_data.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "addons/addon_manager.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include <algorithm>
#include <cstring>
#include <cctype>

using wowee::ui::chat_utils::trim;
using wowee::ui::chat_utils::toLower;

namespace {

    bool isPortBotTarget(const std::string& target) {
        std::string t = toLower(trim(target));
        return t == "portbot" || t == "gmbot" || t == "telebot";
    }

    std::string buildPortBotCommand(const std::string& rawInput) {
        std::string input = trim(rawInput);
        if (input.empty()) return "";

        std::string lower = toLower(input);
        if (lower == "help" || lower == "?") {
            return "__help__";
        }

        if (lower.rfind(".tele ", 0) == 0 || lower.rfind(".go ", 0) == 0) {
            return input;
        }

        if (lower.rfind("xyz ", 0) == 0) {
            return ".go " + input;
        }

        if (lower == "sw" || lower == "stormwind") return ".tele stormwind";
        if (lower == "if" || lower == "ironforge") return ".tele ironforge";
        if (lower == "darn" || lower == "darnassus") return ".tele darnassus";
        if (lower == "org" || lower == "orgrimmar") return ".tele orgrimmar";
        if (lower == "tb" || lower == "thunderbluff") return ".tele thunderbluff";
        if (lower == "uc" || lower == "undercity") return ".tele undercity";
        if (lower == "shatt" || lower == "shattrath") return ".tele shattrath";
        if (lower == "dal" || lower == "dalaran") return ".tele dalaran";

        return ".tele " + input;
    }

    std::string getEntityName(const std::shared_ptr<wowee::game::Entity>& entity) {
        if (entity->getType() == wowee::game::ObjectType::PLAYER) {
            auto player = std::static_pointer_cast<wowee::game::Player>(entity);
            if (!player->getName().empty()) return player->getName();
        } else if (entity->getType() == wowee::game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<wowee::game::Unit>(entity);
            if (!unit->getName().empty()) return unit->getName();
        } else if (entity->getType() == wowee::game::ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<wowee::game::GameObject>(entity);
            if (!go->getName().empty()) return go->getName();
        }
        return "Unknown";
    }
} // namespace

namespace wowee { namespace ui {

static std::vector<std::string> allMacroCommands(const std::string& macroText) {
    std::vector<std::string> cmds;
    size_t pos = 0;
    while (pos <= macroText.size()) {
        size_t nl = macroText.find('\n', pos);
        std::string line = (nl != std::string::npos) ? macroText.substr(pos, nl - pos) : macroText.substr(pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos) line = line.substr(start);
        if (!line.empty() && line.front() != '#')
            cmds.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return cmds;
}

// ---------------------------------------------------------------------------
// evaluateMacroConditionals — thin wrapper over MacroEvaluator (Phase 4).
// Preserved for backward compatibility with command files that forward-declare it.
// ---------------------------------------------------------------------------
std::string evaluateMacroConditionals(const std::string& rawArg,
                                              game::GameHandler& gameHandler,
                                              uint64_t& targetOverride) {
    auto* renderer = core::Application::getInstance().getRenderer();
    GameStateAdapter gs(gameHandler, renderer);
    InputModifierAdapter im;
    MacroEvaluator eval(gs, im);
    return eval.evaluate(rawArg, targetOverride);
}

// Execute all non-comment lines of a macro body in sequence.
// In WoW, every line executes per click; the server enforces spell-cast limits.
// /stopmacro (with optional conditionals) halts the remaining commands early.

void ChatPanel::executeMacroText(game::GameHandler& gameHandler,
                                  const std::string& macroText) {
    macroStopped_ = false;
    for (const auto& cmd : allMacroCommands(macroText)) {
        strncpy(chatInputBuffer_, cmd.c_str(), sizeof(chatInputBuffer_) - 1);
        chatInputBuffer_[sizeof(chatInputBuffer_) - 1] = '\0';
        sendChatMessage(gameHandler);
        if (macroStopped_) break;
    }
    macroStopped_ = false;
}

// /castsequence state moved to CastSequenceTracker member (Phase 1.5)


void ChatPanel::sendChatMessage(game::GameHandler& gameHandler) {
    if (strlen(chatInputBuffer_) > 0) {
        std::string input(chatInputBuffer_);

        // Save to sent-message history (skip pure whitespace, cap at 50 entries)
        {
            bool allSpace = true;
            for (char c : input) { if (!std::isspace(static_cast<unsigned char>(c))) { allSpace = false; break; } }
            if (!allSpace) {
                // Remove duplicate of last entry if identical
                if (chatSentHistory_.empty() || chatSentHistory_.back() != input) {
                    chatSentHistory_.push_back(input);
                    if (chatSentHistory_.size() > 50)
                        chatSentHistory_.erase(chatSentHistory_.begin());
                }
            }
        }
        chatHistoryIdx_ = -1;  // reset browsing position after send

        game::ChatType type = game::ChatType::SAY;
        std::string message = input;
        std::string target;

        // GM dot-prefix commands (.gm, .tele, .additem, etc.)
        // Sent to server as SAY — the server interprets the dot-prefix.
        // Requires GM security level on the server (account set gmlevel <user> 3 -1).
        if (input.size() > 1 && input[0] == '.') {
            LOG_INFO("GM command: '", input, "' — sending as SAY to server");
            gameHandler.sendChatMessage(game::ChatType::SAY, input, "");

            // Build feedback: check if this is a known command
            std::string dotCmd = input;
            size_t sp = dotCmd.find(' ');
            std::string cmdPart = (sp != std::string::npos)
                ? dotCmd.substr(1, sp - 1) : dotCmd.substr(1);
            for (char& c : cmdPart) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Look for a matching entry in the GM command table
            std::string feedback;
            for (const auto& entry : kGmCommands) {
                if (entry.name == cmdPart) {
                    feedback = "Sent: " + input + "  (" + std::string(entry.help) + ")";
                    break;
                }
            }
            if (feedback.empty())
                feedback = "Sent: " + input
                    + "  (requires GM access — server console: account set gmlevel <user> 3 -1)";
            gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(feedback));
            chatInputBuffer_[0] = '\0';
            return;
        }

        // Check for slash commands
        if (input.size() > 1 && input[0] == '/') {
            std::string command = input.substr(1);
            size_t spacePos = command.find(' ');
            std::string cmd = (spacePos != std::string::npos) ? command.substr(0, spacePos) : command;

            // Convert command to lowercase for comparison
            std::string cmdLower = cmd;
            for (char& c : cmdLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // /run <lua code> — execute Lua script via addon system
            if ((cmdLower == "run" || cmdLower == "script") && spacePos != std::string::npos) {
                std::string luaCode = command.substr(spacePos + 1);
                auto* am = services_.addonManager;
                if (am) {
                    am->runScript(luaCode);
                } else {
                    gameHandler.addUIError("Addon system not initialized.");
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /dump <expression> — evaluate Lua expression and print result
            if ((cmdLower == "dump" || cmdLower == "print") && spacePos != std::string::npos) {
                std::string expr = command.substr(spacePos + 1);
                auto* am = services_.addonManager;
                if (am && am->isInitialized()) {
                    // Wrap expression in print(tostring(...)) to display the value
                    std::string wrapped = "local __v = " + expr +
                        "; if type(__v) == 'table' then "
                        "  local parts = {} "
                        "  for k,v in pairs(__v) do parts[#parts+1] = tostring(k)..'='..tostring(v) end "
                        "  print('{' .. table.concat(parts, ', ') .. '}') "
                        "else print(tostring(__v)) end";
                    am->runScript(wrapped);
                } else {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.language = game::ChatLanguage::UNIVERSAL;
                    errMsg.message = "Addon system not initialized.";
                    gameHandler.addLocalChatMessage(errMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Check addon slash commands (SlashCmdList) before built-in commands
            {
                auto* am = services_.addonManager;
                if (am && am->isInitialized()) {
                    std::string slashCmd = "/" + cmdLower;
                    std::string slashArgs;
                    if (spacePos != std::string::npos) slashArgs = command.substr(spacePos + 1);
                    if (am->getLuaEngine()->dispatchSlashCommand(slashCmd, slashArgs)) {
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }
            }

            // Dispatch through command registry (Phase 3.11)
            std::string args;
            if (spacePos != std::string::npos)
                args = command.substr(spacePos + 1);

            ChatCommandContext ctx{gameHandler, services_, *this, args, cmdLower};
            ChatCommandResult result = commandRegistry_.dispatch(cmdLower, ctx);

            if (result.handled) {
                if (result.clearInput)
                    chatInputBuffer_[0] = '\0';
                return;
            }

            // Emote fallthrough — dynamic DBC lookup for emote text (catch-all).
            // Not registered in the command registry because emote names are data-driven.
            {
                std::string targetName;
                const std::string* targetNamePtr = nullptr;
                if (gameHandler.hasTarget()) {
                    auto targetEntity = gameHandler.getTarget();
                    if (targetEntity) {
                        targetName = getEntityName(targetEntity);
                        if (!targetName.empty()) targetNamePtr = &targetName;
                    }
                }

                std::string emoteText = rendering::AnimationController::getEmoteText(cmdLower, targetNamePtr);
                if (!emoteText.empty()) {
                    auto* renderer = services_.renderer;
                    if (renderer) {
                        if (auto* ac = renderer->getAnimationController()) ac->playEmote(cmdLower);
                    }

                    uint32_t dbcId = rendering::AnimationController::getEmoteDbcId(cmdLower);
                    if (dbcId != 0) {
                        uint64_t targetGuid = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                        gameHandler.sendTextEmote(dbcId, targetGuid);
                    }

                    game::MessageChatData msg;
                    msg.type = game::ChatType::TEXT_EMOTE;
                    msg.language = game::ChatLanguage::COMMON;
                    msg.message = emoteText;
                    gameHandler.addLocalChatMessage(msg);

                    chatInputBuffer_[0] = '\0';
                    return;
                }
            }

            // Unrecognized slash command — fall through to dropdown chat type
            message = input;
        }

        // Determine chat type from dropdown selection
        // (reached when: no slash prefix, OR unrecognized slash command)
        switch (selectedChatType_) {
            case 0: type = game::ChatType::SAY; break;
            case 1: type = game::ChatType::YELL; break;
            case 2: type = game::ChatType::PARTY; break;
            case 3: type = game::ChatType::GUILD; break;
            case 4: type = game::ChatType::WHISPER; target = whisperTargetBuffer_; break;
            case 5: type = game::ChatType::RAID; break;
            case 6: type = game::ChatType::OFFICER; break;
            case 7: type = game::ChatType::BATTLEGROUND; break;
            case 8: type = game::ChatType::RAID_WARNING; break;
            case 9: type = game::ChatType::PARTY; break; // INSTANCE uses PARTY
            case 10: { // CHANNEL
                const auto& chans = gameHandler.getJoinedChannels();
                if (!chans.empty() && selectedChannelIdx_ < static_cast<int>(chans.size())) {
                    type = game::ChatType::CHANNEL;
                    target = chans[selectedChannelIdx_];
                } else { type = game::ChatType::SAY; }
                break;
            }
            default: type = game::ChatType::SAY; break;
        }

        // PortBot whisper interception (for dropdown-typed whispers, not /w command)
        if (type == game::ChatType::WHISPER && isPortBotTarget(target)) {
            std::string cmd = buildPortBotCommand(message);
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            if (cmd.empty() || cmd == "__help__") {
                msg.message = "PortBot: /w PortBot <dest>. Aliases: sw if darn org tb uc shatt dal. Also supports '.tele ...' or 'xyz x y z [map [o]]'.";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            gameHandler.sendChatMessage(game::ChatType::SAY, cmd, "");
            msg.message = "PortBot executed: " + cmd;
            gameHandler.addLocalChatMessage(msg);
            chatInputBuffer_[0] = '\0';
            return;
        }

        // Validate whisper has a target
        if (type == game::ChatType::WHISPER && target.empty()) {
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = "You must specify a player name for whisper.";
            gameHandler.addLocalChatMessage(msg);
            chatInputBuffer_[0] = '\0';
            return;
        }

        // Don't send empty messages
        if (!message.empty()) {
            gameHandler.sendChatMessage(type, message, target);
        }

        // Clear input
        chatInputBuffer_[0] = '\0';
    }
}



} // namespace ui
} // namespace wowee
