#include "ui/chat_panel.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/quest_log_screen.hpp"
#include "ui/ui_colors.hpp"
#include "ui/chat/gm_command_data.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <ctime>
#include <unordered_set>
#include <unordered_map>

namespace {
    // Common ImGui colors (aliases)
    using namespace wowee::ui::colors;
    constexpr auto& kColorRed        = kRed;
    constexpr auto& kColorGreen      = kGreen;
    constexpr auto& kColorBrightGreen= kBrightGreen;
    constexpr auto& kColorYellow     = kYellow;
    constexpr auto& kColorGray       = kGray;
    constexpr auto& kColorDarkGray   = kDarkGray;

    // Common ImGui window flags for popup dialogs
    const ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
}

namespace wowee { namespace ui {

ChatPanel::ChatPanel() {
    // ChatTabManager constructor handles tab initialization
    registerAllCommands();
}

// Tab init and filtering moved to ChatTabManager (Phase 1.3)


void ChatPanel::render(game::GameHandler& gameHandler,
                       InventoryScreen& inventoryScreen,
                       SpellbookScreen& spellbookScreen,
                       QuestLogScreen& questLogScreen) {
    // Cache game handler for input callback lambda (player name tab-completion)
    cachedGameHandler_ = &gameHandler;

    auto* window = services_.window;
    auto* assetMgr = services_.assetManager;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;
    float chatW = std::min(500.0f, screenW * 0.4f);
    float chatH = 220.0f;
    float chatX = 8.0f;
    float chatY = screenH - chatH - 80.0f;  // Above action bar
    if (chatWindowLocked_) {
        // Always recompute position from current window size when locked
        chatWindowPos_ = ImVec2(chatX, chatY);
        ImGui::SetNextWindowSize(ImVec2(chatW, chatH), ImGuiCond_Always);
        ImGui::SetNextWindowPos(chatWindowPos_, ImGuiCond_Always);
    } else {
        if (!chatWindowPosInit_) {
            chatWindowPos_ = ImVec2(chatX, chatY);
            chatWindowPosInit_ = true;
        }
        ImGui::SetNextWindowSize(ImVec2(chatW, chatH), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(chatWindowPos_, ImGuiCond_FirstUseEver);
    }
    ImGuiWindowFlags flags = kDialogFlags | ImGuiWindowFlags_NoNavInputs;
    if (chatWindowLocked_) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
    }
    ImGui::Begin("Chat", nullptr, flags);

    if (!chatWindowLocked_) {
        chatWindowPos_ = ImGui::GetWindowPos();
    }

    // Update unread counts via ChatTabManager (Phase 1.3)
    tabManager_.updateUnread(gameHandler.getChatHistory(), activeChatTab);

    // Chat tabs (rendered via ChatTabManager)
    if (ImGui::BeginTabBar("ChatTabs")) {
        for (int i = 0; i < tabManager_.getTabCount(); ++i) {
            // Build label with unread count suffix for non-General tabs
            std::string tabLabel = tabManager_.getTabName(i);
            int unread = tabManager_.getUnreadCount(i);
            if (i > 0 && unread > 0) {
                tabLabel += " (" + std::to_string(unread) + ")";
            }
            // Flash tab text color when unread messages exist
            bool hasUnread = (i > 0 && unread > 0);
            if (hasUnread) {
                float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f * pulse, 0.2f * pulse, 1.0f));
            }
            if (ImGui::BeginTabItem(tabLabel.c_str())) {
                if (activeChatTab != i) {
                    activeChatTab = i;
                    // Clear unread count when tab becomes active
                    tabManager_.clearUnread(i);
                }
                ImGui::EndTabItem();
            }
            if (hasUnread) ImGui::PopStyleColor();
        }
        ImGui::EndTabBar();
    }

    // Chat history
    const auto& chatHistory = gameHandler.getChatHistory();

    // Apply chat font size scaling
    float chatScale = chatFontSize == 0 ? 0.85f : (chatFontSize == 2 ? 1.2f : 1.0f);
    ImGui::SetWindowFontScale(chatScale);

    ImGui::BeginChild("ChatHistory", ImVec2(0, -70), true, ImGuiWindowFlags_HorizontalScrollbar);
    bool chatHistoryHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Markup parsing and rendering delegated to ChatMarkupParser / ChatMarkupRenderer (Phase 2)
    MarkupRenderContext markupCtx;
    markupCtx.gameHandler     = &gameHandler;
    markupCtx.inventory       = &inventoryScreen;
    markupCtx.spellbook       = &spellbookScreen;
    markupCtx.questLog        = &questLogScreen;
    markupCtx.assetMgr        = assetMgr;
    markupCtx.getSpellIcon    = getSpellIcon;
    markupCtx.chatInputBuffer = chatInputBuffer_;
    markupCtx.chatInputBufSize = sizeof(chatInputBuffer_);
    markupCtx.moveCursorToEnd = &chatInputMoveCursorToEnd_;

    // Determine local player name for mention detection (case-insensitive)
    std::string selfNameLower;
    {
        const auto* ch = gameHandler.getActiveCharacter();
        if (ch && !ch->name.empty()) {
            selfNameLower = ch->name;
            for (auto& c : selfNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    // Scan NEW messages (beyond chatMentionSeenCount_) for mentions and play notification sound
    if (!selfNameLower.empty() && chatHistory.size() > chatMentionSeenCount_) {
        for (size_t mi = chatMentionSeenCount_; mi < chatHistory.size(); ++mi) {
            const auto& mMsg = chatHistory[mi];
            // Skip outgoing whispers, system, and monster messages
            if (mMsg.type == game::ChatType::WHISPER_INFORM ||
                mMsg.type == game::ChatType::SYSTEM) continue;
            // Case-insensitive search in message body
            std::string bodyLower = mMsg.message;
            for (auto& c : bodyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (bodyLower.find(selfNameLower) != std::string::npos) {
                if (auto* ac = services_.audioCoordinator) {
                    if (auto* ui = ac->getUiSoundManager())
                        ui->playWhisperReceived();
                }
                break; // play at most once per scan pass
            }
        }
        chatMentionSeenCount_ = chatHistory.size();
    } else if (chatHistory.size() <= chatMentionSeenCount_) {
        chatMentionSeenCount_ = chatHistory.size();  // reset if history was cleared
    }

    // Whisper toast scanning left in GameScreen (will move to ToastManager later)

    int chatMsgIdx = 0;
    for (const auto& msg : chatHistory) {
        if (!tabManager_.shouldShowMessage(msg, activeChatTab)) continue;
        std::string processedMessage = replaceGenderPlaceholders(msg.message, gameHandler);

        // Resolve sender name at render time in case it wasn't available at parse time.
        // This handles the race where SMSG_MESSAGECHAT arrives before the entity spawns.
        const std::string& resolvedSenderName = [&]() -> const std::string& {
            if (!msg.senderName.empty()) return msg.senderName;
            if (msg.senderGuid == 0) return msg.senderName;
            const std::string& cached = gameHandler.lookupName(msg.senderGuid);
            if (!cached.empty()) return cached;
            return msg.senderName;
        }();

        ImVec4 color = ChatTabManager::getChatTypeColor(msg.type);

        // Optional timestamp prefix
        std::string tsPrefix;
        if (chatShowTimestamps) {
            auto tt = std::chrono::system_clock::to_time_t(msg.timestamp);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            char tsBuf[16];
            snprintf(tsBuf, sizeof(tsBuf), "[%02d:%02d] ", tm.tm_hour, tm.tm_min);
            tsPrefix = tsBuf;
        }

        // Build chat tag prefix: <GM>, <AFK>, <DND> from chatTag bitmask
        std::string tagPrefix;
        if (msg.chatTag & 0x04) tagPrefix = "<GM> ";
        else if (msg.chatTag & 0x01) tagPrefix = "<AFK> ";
        else if (msg.chatTag & 0x02) tagPrefix = "<DND> ";

        // Build full message string for this entry
        std::string fullMsg;
        if (msg.type == game::ChatType::SYSTEM || msg.type == game::ChatType::TEXT_EMOTE) {
            fullMsg = tsPrefix + processedMessage;
        } else if (!resolvedSenderName.empty()) {
            if (msg.type == game::ChatType::SAY ||
                msg.type == game::ChatType::MONSTER_SAY || msg.type == game::ChatType::MONSTER_PARTY) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " says: " + processedMessage;
            } else if (msg.type == game::ChatType::YELL || msg.type == game::ChatType::MONSTER_YELL) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " yells: " + processedMessage;
            } else if (msg.type == game::ChatType::WHISPER ||
                       msg.type == game::ChatType::MONSTER_WHISPER || msg.type == game::ChatType::RAID_BOSS_WHISPER) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " whispers: " + processedMessage;
            } else if (msg.type == game::ChatType::WHISPER_INFORM) {
                const std::string& target = !msg.receiverName.empty() ? msg.receiverName : resolvedSenderName;
                fullMsg = tsPrefix + "To " + target + ": " + processedMessage;
            } else if (msg.type == game::ChatType::EMOTE ||
                       msg.type == game::ChatType::MONSTER_EMOTE || msg.type == game::ChatType::RAID_BOSS_EMOTE) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " " + processedMessage;
            } else if (msg.type == game::ChatType::CHANNEL && !msg.channelName.empty()) {
                int chIdx = gameHandler.getChannelIndex(msg.channelName);
                std::string chDisplay = chIdx > 0
                    ? "[" + std::to_string(chIdx) + ". " + msg.channelName + "]"
                    : "[" + msg.channelName + "]";
                fullMsg = tsPrefix + chDisplay + " [" + tagPrefix + resolvedSenderName + "]: " + processedMessage;
            } else {
                fullMsg = tsPrefix + "[" + std::string(ChatTabManager::getChatTypeName(msg.type)) + "] " + tagPrefix + resolvedSenderName + ": " + processedMessage;
            }
        } else {
            bool isGroupType =
                msg.type == game::ChatType::PARTY ||
                msg.type == game::ChatType::GUILD ||
                msg.type == game::ChatType::OFFICER ||
                msg.type == game::ChatType::RAID ||
                msg.type == game::ChatType::RAID_LEADER ||
                msg.type == game::ChatType::RAID_WARNING ||
                msg.type == game::ChatType::BATTLEGROUND ||
                msg.type == game::ChatType::BATTLEGROUND_LEADER;
            if (isGroupType) {
                fullMsg = tsPrefix + "[" + std::string(ChatTabManager::getChatTypeName(msg.type)) + "] " + processedMessage;
            } else {
                fullMsg = tsPrefix + processedMessage;
            }
        }

        // Detect mention: does this message contain the local player's name?
        bool isMention = false;
        if (!selfNameLower.empty() &&
            msg.type != game::ChatType::WHISPER_INFORM &&
            msg.type != game::ChatType::SYSTEM) {
            std::string msgLower = fullMsg;
            for (auto& c : msgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            isMention = (msgLower.find(selfNameLower) != std::string::npos);
        }

        // Render message in a group so we can attach a right-click context menu
        ImGui::PushID(chatMsgIdx++);
        ImGui::BeginGroup();
        {
            auto segments = markupParser_.parse(fullMsg);
            markupRenderer_.render(segments, isMention ? ImVec4(1.0f, 0.9f, 0.35f, 1.0f) : color, markupCtx);
        }
        ImGui::EndGroup();
        if (isMention) {
            // Draw highlight AFTER rendering so the rect covers all wrapped lines,
            // not just the first. Previously used a pre-render single-lineH rect.
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            float availW = ImGui::GetContentRegionAvail().x + ImGui::GetCursorScreenPos().x - rMin.x;
            ImGui::GetWindowDrawList()->AddRectFilled(
                rMin, ImVec2(rMin.x + availW, rMax.y),
                IM_COL32(255, 200, 50, 45));  // soft golden tint
        }

        // Right-click context menu (only for player messages with a sender)
        bool isPlayerMsg = !resolvedSenderName.empty() &&
            msg.type != game::ChatType::SYSTEM &&
            msg.type != game::ChatType::TEXT_EMOTE &&
            msg.type != game::ChatType::MONSTER_SAY &&
            msg.type != game::ChatType::MONSTER_YELL &&
            msg.type != game::ChatType::MONSTER_WHISPER &&
            msg.type != game::ChatType::MONSTER_EMOTE &&
            msg.type != game::ChatType::MONSTER_PARTY &&
            msg.type != game::ChatType::RAID_BOSS_WHISPER &&
            msg.type != game::ChatType::RAID_BOSS_EMOTE;

        if (isPlayerMsg && ImGui::BeginPopupContextItem("ChatMsgCtx")) {
            ImGui::TextDisabled("%s", resolvedSenderName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Whisper")) {
                selectedChatType_ = 4; // WHISPER
                strncpy(whisperTargetBuffer_, resolvedSenderName.c_str(), sizeof(whisperTargetBuffer_) - 1);
                whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                refocusChatInput_ = true;
            }
            if (ImGui::MenuItem("Invite to Group")) {
                gameHandler.inviteToGroup(resolvedSenderName);
            }
            if (ImGui::MenuItem("Add Friend")) {
                gameHandler.addFriend(resolvedSenderName);
            }
            if (ImGui::MenuItem("Ignore")) {
                gameHandler.addIgnore(resolvedSenderName);
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    // Auto-scroll to bottom; track whether user has scrolled up
    {
        float scrollY    = ImGui::GetScrollY();
        float scrollMaxY = ImGui::GetScrollMaxY();
        bool atBottom = (scrollMaxY <= 0.0f) || (scrollY >= scrollMaxY - 2.0f);
        if (atBottom || chatForceScrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            chatScrolledUp_ = false;
            chatForceScrollToBottom_ = false;
        } else {
            chatScrolledUp_ = true;
        }
    }

    ImGui::EndChild();

    // Reset font scale after chat history
    ImGui::SetWindowFontScale(1.0f);

    // "Jump to bottom" indicator when scrolled up
    if (chatScrolledUp_) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.35f, 0.7f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f,  0.9f, 1.0f));
        if (ImGui::SmallButton("  v  New messages  ")) {
            chatForceScrollToBottom_ = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    // Lock toggle
    ImGui::Checkbox("Lock", &chatWindowLocked_);
    ImGui::SameLine();
    ImGui::TextDisabled(chatWindowLocked_ ? "(locked)" : "(movable)");

    // Chat input
    ImGui::Text("Type:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* chatTypes[] = { "SAY", "YELL", "PARTY", "GUILD", "WHISPER", "RAID", "OFFICER", "BATTLEGROUND", "RAID WARNING", "INSTANCE", "CHANNEL" };
    ImGui::Combo("##ChatType", &selectedChatType_, chatTypes, 11);

    // Auto-fill whisper target when switching to WHISPER mode
    if (selectedChatType_ == 4 && lastChatType_ != 4) {
        // Just switched to WHISPER mode
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target && target->getType() == game::ObjectType::PLAYER) {
                auto player = std::static_pointer_cast<game::Player>(target);
                if (!player->getName().empty()) {
                    strncpy(whisperTargetBuffer_, player->getName().c_str(), sizeof(whisperTargetBuffer_) - 1);
                    whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                }
            }
        }
    }
    lastChatType_ = selectedChatType_;

    // Show whisper target field if WHISPER is selected
    if (selectedChatType_ == 4) {
        ImGui::SameLine();
        ImGui::Text("To:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##WhisperTarget", whisperTargetBuffer_, sizeof(whisperTargetBuffer_));
    }

    // Show channel picker if CHANNEL is selected
    if (selectedChatType_ == 10) {
        const auto& channels = gameHandler.getJoinedChannels();
        if (channels.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no channels joined)");
        } else {
            ImGui::SameLine();
            if (selectedChannelIdx_ >= static_cast<int>(channels.size())) selectedChannelIdx_ = 0;
            ImGui::SetNextItemWidth(140);
            if (ImGui::BeginCombo("##ChannelPicker", channels[selectedChannelIdx_].c_str())) {
                for (int ci = 0; ci < static_cast<int>(channels.size()); ++ci) {
                    bool selected = (ci == selectedChannelIdx_);
                    if (ImGui::Selectable(channels[ci].c_str(), selected)) selectedChannelIdx_ = ci;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
    }

    ImGui::SameLine();
    ImGui::Text("Message:");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(-1);
    if (refocusChatInput_) {
        ImGui::SetKeyboardFocusHere();
        refocusChatInput_ = false;
    }

    // Detect chat channel prefix as user types and switch the dropdown
    {
        std::string buf(chatInputBuffer_);
        if (buf.size() >= 2 && buf[0] == '/') {
            // Find the command and check if there's a space after it
            size_t sp = buf.find(' ', 1);
            if (sp != std::string::npos) {
                std::string cmd = buf.substr(1, sp - 1);
                for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                int detected = -1;
                bool isReply = false;
                if (cmd == "s" || cmd == "say") detected = 0;
                else if (cmd == "y" || cmd == "yell" || cmd == "shout") detected = 1;
                else if (cmd == "p" || cmd == "party") detected = 2;
                else if (cmd == "g" || cmd == "guild") detected = 3;
                else if (cmd == "w" || cmd == "whisper" || cmd == "tell" || cmd == "t") detected = 4;
                else if (cmd == "r" || cmd == "reply") { detected = 4; isReply = true; }
                else if (cmd == "raid" || cmd == "rsay" || cmd == "ra") detected = 5;
                else if (cmd == "o" || cmd == "officer" || cmd == "osay") detected = 6;
                else if (cmd == "bg" || cmd == "battleground") detected = 7;
                else if (cmd == "rw" || cmd == "raidwarning") detected = 8;
                else if (cmd == "i" || cmd == "instance") detected = 9;
                else if (cmd.size() == 1 && cmd[0] >= '1' && cmd[0] <= '9') detected = 10; // /1, /2 etc.
                if (detected >= 0 && (selectedChatType_ != detected || detected == 10 || isReply)) {
                    // For channel shortcuts, also update selectedChannelIdx_
                    if (detected == 10) {
                        int chanIdx = cmd[0] - '1'; // /1 -> index 0, /2 -> index 1, etc.
                        const auto& chans = gameHandler.getJoinedChannels();
                        if (chanIdx >= 0 && chanIdx < static_cast<int>(chans.size())) {
                            selectedChannelIdx_ = chanIdx;
                        }
                    }
                    selectedChatType_ = detected;
                    // Strip the prefix, keep only the message part
                    std::string remaining = buf.substr(sp + 1);
                    // /r reply: pre-fill whisper target from last whisper sender
                    if (detected == 4 && isReply) {
                        std::string lastSender = gameHandler.getLastWhisperSender();
                        if (!lastSender.empty()) {
                            strncpy(whisperTargetBuffer_, lastSender.c_str(), sizeof(whisperTargetBuffer_) - 1);
                            whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                        }
                        // remaining is the message — don't extract a target from it
                    } else if (detected == 4) {
                        // For whisper, first word after /w is the target
                        size_t msgStart = remaining.find(' ');
                        if (msgStart != std::string::npos) {
                            std::string wTarget = remaining.substr(0, msgStart);
                            strncpy(whisperTargetBuffer_, wTarget.c_str(), sizeof(whisperTargetBuffer_) - 1);
                            whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                            remaining = remaining.substr(msgStart + 1);
                        } else {
                            // Just the target name so far, no message yet
                            strncpy(whisperTargetBuffer_, remaining.c_str(), sizeof(whisperTargetBuffer_) - 1);
                            whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                            remaining = "";
                        }
                    }
                    strncpy(chatInputBuffer_, remaining.c_str(), sizeof(chatInputBuffer_) - 1);
                    chatInputBuffer_[sizeof(chatInputBuffer_) - 1] = '\0';
                    chatInputMoveCursorToEnd_ = true;
                }
            }
        }
    }

    // Color the input text based on current chat type
    ImVec4 inputColor;
    switch (selectedChatType_) {
        case 1: inputColor = kColorRed; break;  // YELL - red
        case 2: inputColor = colors::kLightBlue; break;  // PARTY - blue
        case 3: inputColor = kColorBrightGreen; break;  // GUILD - green
        case 4: inputColor = ImVec4(1.0f, 0.5f, 1.0f, 1.0f); break;  // WHISPER - pink
        case 5: inputColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;  // RAID - orange
        case 6: inputColor = kColorBrightGreen; break;  // OFFICER - green
        case 7: inputColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;  // BG - orange
        case 8: inputColor = ImVec4(1.0f, 0.3f, 0.0f, 1.0f); break;  // RAID WARNING - red-orange
        case 9:  inputColor = colors::kLightBlue; break;  // INSTANCE - blue
        case 10: inputColor = ImVec4(0.3f, 0.9f, 0.9f, 1.0f); break; // CHANNEL - cyan
        default: inputColor = ui::colors::kWhite; break; // SAY - white
    }
    ImGui::PushStyleColor(ImGuiCol_Text, inputColor);

    auto inputCallback = [](ImGuiInputTextCallbackData* data) -> int {
        auto* self = static_cast<ChatPanel*>(data->UserData);
        if (!self) return 0;

        // Cursor-to-end after channel switch
        if (self->chatInputMoveCursorToEnd_) {
            int len = static_cast<int>(std::strlen(data->Buf));
            data->CursorPos = len;
            data->SelectionStart = len;
            data->SelectionEnd = len;
            self->chatInputMoveCursorToEnd_ = false;
        }

        // Tab: slash-command autocomplete (Phase 5 — uses ChatTabCompleter + ChatCommandRegistry)
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            if (data->BufTextLen > 0 && data->Buf[0] == '/') {
                // Split buffer into command word and trailing args
                std::string fullBuf(data->Buf, data->BufTextLen);
                size_t spacePos = fullBuf.find(' ');
                std::string word = (spacePos != std::string::npos) ? fullBuf.substr(0, spacePos) : fullBuf;
                std::string rest = (spacePos != std::string::npos) ? fullBuf.substr(spacePos) : "";

                // Normalize to lowercase for matching (strip leading /)
                std::string lowerCmd = word.substr(1);
                for (auto& ch : lowerCmd) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

                // New session if prefix changed
                if (!self->tabCompleter_.isActive() || self->tabCompleter_.getPrefix() != lowerCmd) {
                    auto candidates = self->commandRegistry_.getCompletions(lowerCmd);
                    // Prepend / to each candidate for display
                    for (auto& c : candidates) c = "/" + c;
                    self->tabCompleter_.startCompletion(lowerCmd, std::move(candidates));
                } else {
                    self->tabCompleter_.next();
                }

                std::string match = self->tabCompleter_.getCurrentMatch();
                if (!match.empty()) {
                    // Append trailing space when match is unambiguous
                    if (self->tabCompleter_.matchCount() == 1 && rest.empty())
                        match += ' ';
                    std::string newBuf = match + rest;
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, newBuf.c_str());
                }
            } else if (data->BufTextLen > 1 && data->Buf[0] == '.') {
                // GM dot-command tab-completion (uses gm_command_data.hpp table)
                std::string fullBuf(data->Buf, data->BufTextLen);
                size_t spacePos = fullBuf.find(' ');
                std::string word = (spacePos != std::string::npos) ? fullBuf.substr(0, spacePos) : fullBuf;
                std::string rest = (spacePos != std::string::npos) ? fullBuf.substr(spacePos) : "";

                // Lowercase the typed prefix (keep the dot)
                std::string lowerDot = word;
                for (auto& ch : lowerDot) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

                if (!self->tabCompleter_.isActive() || self->tabCompleter_.getPrefix() != lowerDot) {
                    // Gather candidates from the GM command table
                    std::vector<std::string> candidates;
                    for (const auto& entry : kGmCommands) {
                        std::string dotName = "." + std::string(entry.name);
                        if (dotName.size() >= lowerDot.size() &&
                            dotName.compare(0, lowerDot.size(), lowerDot) == 0) {
                            candidates.push_back(dotName);
                        }
                    }
                    std::sort(candidates.begin(), candidates.end());
                    self->tabCompleter_.startCompletion(lowerDot, std::move(candidates));
                } else {
                    self->tabCompleter_.next();
                }

                std::string match = self->tabCompleter_.getCurrentMatch();
                if (!match.empty()) {
                    if (self->tabCompleter_.matchCount() == 1 && rest.empty())
                        match += ' ';
                    std::string newBuf = match + rest;
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, newBuf.c_str());
                }
            } else if (data->BufTextLen > 0) {
                // Player name tab-completion for commands like /w, /whisper, /invite, /trade, /duel
                std::string fullBuf(data->Buf, data->BufTextLen);
                size_t spacePos = fullBuf.find(' ');
                bool isNameCommand = false;
                std::string namePrefix;
                size_t replaceStart = 0;

                if (fullBuf[0] == '/' && spacePos != std::string::npos) {
                    std::string cmd = fullBuf.substr(0, spacePos);
                    for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (cmd == "/w" || cmd == "/whisper" || cmd == "/invite" ||
                        cmd == "/trade" || cmd == "/duel" || cmd == "/follow" ||
                        cmd == "/inspect" || cmd == "/friend" || cmd == "/removefriend" ||
                        cmd == "/ignore" || cmd == "/unignore" || cmd == "/who" ||
                        cmd == "/t" || cmd == "/target" || cmd == "/kick" ||
                        cmd == "/uninvite" || cmd == "/ginvite" || cmd == "/gkick") {
                        namePrefix = fullBuf.substr(spacePos + 1);
                        size_t nameSpace = namePrefix.find(' ');
                        if (nameSpace == std::string::npos) {
                            isNameCommand = true;
                            replaceStart = spacePos + 1;
                        }
                    }
                }

                if (isNameCommand && !namePrefix.empty()) {
                    std::string lowerPrefix = namePrefix;
                    for (char& c : lowerPrefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    if (!self->tabCompleter_.isActive() || self->tabCompleter_.getPrefix() != lowerPrefix) {
                        std::vector<std::string> candidates;
                        auto* gh = self->cachedGameHandler_;
                        // Party/raid members
                        for (const auto& m : gh->getPartyData().members) {
                            if (m.name.empty()) continue;
                            std::string lname = m.name;
                            for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0)
                                candidates.push_back(m.name);
                        }
                        // Friends
                        for (const auto& c : gh->getContacts()) {
                            if (!c.isFriend() || c.name.empty()) continue;
                            std::string lname = c.name;
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                bool dup = false;
                                for (const auto& em : candidates)
                                    if (em == c.name) { dup = true; break; }
                                if (!dup) candidates.push_back(c.name);
                            }
                        }
                        // Nearby visible players
                        for (const auto& [guid, entity] : gh->getEntityManager().getEntities()) {
                            if (!entity || entity->getType() != game::ObjectType::PLAYER) continue;
                            auto player = std::static_pointer_cast<game::Player>(entity);
                            if (player->getName().empty()) continue;
                            std::string lname = player->getName();
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                bool dup = false;
                                for (const auto& em : candidates)
                                    if (em == player->getName()) { dup = true; break; }
                                if (!dup) candidates.push_back(player->getName());
                            }
                        }
                        // Last whisper sender (insert at front for priority)
                        if (!gh->getLastWhisperSender().empty()) {
                            std::string lname = gh->getLastWhisperSender();
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                bool dup = false;
                                for (const auto& em : candidates)
                                    if (em == gh->getLastWhisperSender()) { dup = true; break; }
                                if (!dup) candidates.insert(candidates.begin(), gh->getLastWhisperSender());
                            }
                        }
                        self->tabCompleter_.startCompletion(lowerPrefix, std::move(candidates));
                    } else {
                        self->tabCompleter_.next();
                    }

                    std::string match = self->tabCompleter_.getCurrentMatch();
                    if (!match.empty()) {
                        std::string prefix = fullBuf.substr(0, replaceStart);
                        std::string newBuf = prefix + match;
                        if (self->tabCompleter_.matchCount() == 1) newBuf += ' ';
                        data->DeleteChars(0, data->BufTextLen);
                        data->InsertChars(0, newBuf.c_str());
                    }
                }
            }
            return 0;
        }

        // Up/Down arrow: cycle through sent message history
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            // Any history navigation resets autocomplete
            self->tabCompleter_.reset();

            const int histSize = static_cast<int>(self->chatSentHistory_.size());
            if (histSize == 0) return 0;

            if (data->EventKey == ImGuiKey_UpArrow) {
                // Go back in history
                if (self->chatHistoryIdx_ == -1)
                    self->chatHistoryIdx_ = histSize - 1;
                else if (self->chatHistoryIdx_ > 0)
                    --self->chatHistoryIdx_;
            } else if (data->EventKey == ImGuiKey_DownArrow) {
                if (self->chatHistoryIdx_ == -1) return 0;
                ++self->chatHistoryIdx_;
                if (self->chatHistoryIdx_ >= histSize) {
                    self->chatHistoryIdx_ = -1;
                    data->DeleteChars(0, data->BufTextLen);
                    return 0;
                }
            }

            if (self->chatHistoryIdx_ >= 0 && self->chatHistoryIdx_ < histSize) {
                const std::string& entry = self->chatSentHistory_[self->chatHistoryIdx_];
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, entry.c_str());
            }
        }
        return 0;
    };

    ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_CallbackAlways |
                                     ImGuiInputTextFlags_CallbackHistory |
                                     ImGuiInputTextFlags_CallbackCompletion;
    if (ImGui::InputText("##ChatInput", chatInputBuffer_, sizeof(chatInputBuffer_), inputFlags, inputCallback, this)) {
        sendChatMessage(gameHandler);
        // Close chat input on send so movement keys work immediately.
        refocusChatInput_ = false;
        chatInputCooldown_ = 2;  // suppress Enter re-opening chat for 2 frames
        ImGui::ClearActiveID();
    }
    ImGui::PopStyleColor();

    if (chatInputCooldown_ > 0) --chatInputCooldown_;

    if (ImGui::IsItemActive()) {
        chatInputActive_ = true;
    } else {
        chatInputActive_ = false;
    }

    // Click in chat history area (received messages) → focus input.
    {
        if (chatHistoryHovered && ImGui::IsMouseClicked(0)) {
            refocusChatInput_ = true;
        }
    }

    ImGui::End();
}

// --- Command registration (calls into each command group file) ---
// Forward declarations of registration functions from command files
void registerSystemCommands(ChatCommandRegistry& reg);
void registerSocialCommands(ChatCommandRegistry& reg);
void registerChannelCommands(ChatCommandRegistry& reg);
void registerCombatCommands(ChatCommandRegistry& reg);
void registerGroupCommands(ChatCommandRegistry& reg);
void registerGuildCommands(ChatCommandRegistry& reg);
void registerTargetCommands(ChatCommandRegistry& reg);
void registerEmoteCommands(ChatCommandRegistry& reg);
void registerMiscCommands(ChatCommandRegistry& reg);
void registerHelpCommands(ChatCommandRegistry& reg);
void registerGmCommands(ChatCommandRegistry& reg);

void ChatPanel::registerAllCommands() {
    registerSystemCommands(commandRegistry_);
    registerSocialCommands(commandRegistry_);
    registerChannelCommands(commandRegistry_);
    registerCombatCommands(commandRegistry_);
    registerGroupCommands(commandRegistry_);
    registerGuildCommands(commandRegistry_);
    registerTargetCommands(commandRegistry_);
    registerEmoteCommands(commandRegistry_);
    registerMiscCommands(commandRegistry_);
    registerHelpCommands(commandRegistry_);
    registerGmCommands(commandRegistry_);
}


// Collect all non-comment, non-empty lines from a macro body.

} // namespace ui
} // namespace wowee
