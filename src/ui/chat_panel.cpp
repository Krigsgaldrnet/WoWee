#include "ui/chat_panel.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/quest_log_screen.hpp"
#include "ui/ui_colors.hpp"
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
    initChatTabs();
}

void ChatPanel::initChatTabs() {
    chatTabs_.clear();
    // General tab: shows everything
    chatTabs_.push_back({"General", ~0ULL});
    // Combat tab: system, loot, skills, achievements, and NPC speech/emotes
    chatTabs_.push_back({"Combat", (1ULL << static_cast<uint8_t>(game::ChatType::SYSTEM)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::LOOT)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::SKILL)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::ACHIEVEMENT)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::GUILD_ACHIEVEMENT)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_SAY)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_YELL)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_EMOTE)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_WHISPER)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_PARTY)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::RAID_BOSS_WHISPER)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::RAID_BOSS_EMOTE))});
    // Whispers tab
    chatTabs_.push_back({"Whispers", (1ULL << static_cast<uint8_t>(game::ChatType::WHISPER)) |
                                      (1ULL << static_cast<uint8_t>(game::ChatType::WHISPER_INFORM))});
    // Guild tab: guild and officer chat
    chatTabs_.push_back({"Guild", (1ULL << static_cast<uint8_t>(game::ChatType::GUILD)) |
                                   (1ULL << static_cast<uint8_t>(game::ChatType::OFFICER)) |
                                   (1ULL << static_cast<uint8_t>(game::ChatType::GUILD_ACHIEVEMENT))});
    // Trade/LFG tab: channel messages
    chatTabs_.push_back({"Trade/LFG", (1ULL << static_cast<uint8_t>(game::ChatType::CHANNEL))});
    // Reset unread counts to match new tab list
    chatTabUnread_.assign(chatTabs_.size(), 0);
    chatTabSeenCount_ = 0;
}

bool ChatPanel::shouldShowMessage(const game::MessageChatData& msg, int tabIndex) const {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(chatTabs_.size())) return true;
    const auto& tab = chatTabs_[tabIndex];
    if (tab.typeMask == ~0ULL) return true;  // General tab shows all

    uint64_t typeBit = 1ULL << static_cast<uint8_t>(msg.type);

    // For Trade/LFG tab (now index 4), also filter by channel name
    if (tabIndex == 4 && msg.type == game::ChatType::CHANNEL) {
        const std::string& ch = msg.channelName;
        if (ch.find("Trade") == std::string::npos &&
            ch.find("General") == std::string::npos &&
            ch.find("LookingForGroup") == std::string::npos &&
            ch.find("Local") == std::string::npos) {
            return false;
        }
        return true;
    }

    return (tab.typeMask & typeBit) != 0;
}


void ChatPanel::render(game::GameHandler& gameHandler,
                       InventoryScreen& inventoryScreen,
                       SpellbookScreen& spellbookScreen,
                       QuestLogScreen& questLogScreen) {
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
    ImGuiWindowFlags flags = kDialogFlags;
    if (chatWindowLocked_) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
    }
    ImGui::Begin("Chat", nullptr, flags);

    if (!chatWindowLocked_) {
        chatWindowPos_ = ImGui::GetWindowPos();
    }

    // Update unread counts: scan any new messages since last frame
    {
        const auto& history = gameHandler.getChatHistory();
        // Ensure unread array is sized correctly (guards against late init)
        if (chatTabUnread_.size() != chatTabs_.size())
            chatTabUnread_.assign(chatTabs_.size(), 0);
        // If history shrank (e.g. cleared), reset
        if (chatTabSeenCount_ > history.size()) chatTabSeenCount_ = 0;
        for (size_t mi = chatTabSeenCount_; mi < history.size(); ++mi) {
            const auto& msg = history[mi];
            // For each non-General (non-0) tab that isn't currently active, check visibility
            for (int ti = 1; ti < static_cast<int>(chatTabs_.size()); ++ti) {
                if (ti == activeChatTab) continue;
                if (shouldShowMessage(msg, ti)) {
                    chatTabUnread_[ti]++;
                }
            }
        }
        chatTabSeenCount_ = history.size();
    }

    // Chat tabs
    if (ImGui::BeginTabBar("ChatTabs")) {
        for (int i = 0; i < static_cast<int>(chatTabs_.size()); ++i) {
            // Build label with unread count suffix for non-General tabs
            std::string tabLabel = chatTabs_[i].name;
            if (i > 0 && i < static_cast<int>(chatTabUnread_.size()) && chatTabUnread_[i] > 0) {
                tabLabel += " (" + std::to_string(chatTabUnread_[i]) + ")";
            }
            // Flash tab text color when unread messages exist
            bool hasUnread = (i > 0 && i < static_cast<int>(chatTabUnread_.size()) && chatTabUnread_[i] > 0);
            if (hasUnread) {
                float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f * pulse, 0.2f * pulse, 1.0f));
            }
            if (ImGui::BeginTabItem(tabLabel.c_str())) {
                if (activeChatTab != i) {
                    activeChatTab = i;
                    // Clear unread count when tab becomes active
                    if (i < static_cast<int>(chatTabUnread_.size()))
                        chatTabUnread_[i] = 0;
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

    // Helper: parse WoW color code |cAARRGGBB → ImVec4
    auto parseWowColor = [](const std::string& text, size_t pos) -> ImVec4 {
        // |cAARRGGBB (10 chars total: |c + 8 hex)
        if (pos + 10 > text.size()) return colors::kWhite;
        auto hexByte = [&](size_t offset) -> float {
            const char* s = text.c_str() + pos + offset;
            char buf[3] = {s[0], s[1], '\0'};
            return static_cast<float>(strtol(buf, nullptr, 16)) / 255.0f;
        };
        float a = hexByte(2);
        float r = hexByte(4);
        float g = hexByte(6);
        float b = hexByte(8);
        return ImVec4(r, g, b, a);
    };

    // Helper: render an item tooltip from ItemQueryResponseData
    auto renderItemLinkTooltip = [&](uint32_t itemEntry) {
        const auto* info = gameHandler.getItemInfo(itemEntry);
        if (!info || !info->valid) return;
        auto findComparableEquipped = [&](uint8_t inventoryType) -> const game::ItemSlot* {
            using ES = game::EquipSlot;
            const auto& inv = gameHandler.getInventory();
            auto slotPtr = [&](ES slot) -> const game::ItemSlot* {
                const auto& s = inv.getEquipSlot(slot);
                return s.empty() ? nullptr : &s;
            };
            switch (inventoryType) {
                case 1: return slotPtr(ES::HEAD);
                case 2: return slotPtr(ES::NECK);
                case 3: return slotPtr(ES::SHOULDERS);
                case 4: return slotPtr(ES::SHIRT);
                case 5:
                case 20: return slotPtr(ES::CHEST);
                case 6: return slotPtr(ES::WAIST);
                case 7: return slotPtr(ES::LEGS);
                case 8: return slotPtr(ES::FEET);
                case 9: return slotPtr(ES::WRISTS);
                case 10: return slotPtr(ES::HANDS);
                case 11: {
                    if (auto* s = slotPtr(ES::RING1)) return s;
                    return slotPtr(ES::RING2);
                }
                case 12: {
                    if (auto* s = slotPtr(ES::TRINKET1)) return s;
                    return slotPtr(ES::TRINKET2);
                }
                case 13:
                    if (auto* s = slotPtr(ES::MAIN_HAND)) return s;
                    return slotPtr(ES::OFF_HAND);
                case 14:
                case 22:
                case 23: return slotPtr(ES::OFF_HAND);
                case 15:
                case 25:
                case 26: return slotPtr(ES::RANGED);
                case 16: return slotPtr(ES::BACK);
                case 17:
                case 21: return slotPtr(ES::MAIN_HAND);
                case 18:
                    for (int i = 0; i < game::Inventory::NUM_BAG_SLOTS; ++i) {
                        auto slot = static_cast<ES>(static_cast<int>(ES::BAG1) + i);
                        if (auto* s = slotPtr(slot)) return s;
                    }
                    return nullptr;
                case 19: return slotPtr(ES::TABARD);
                default: return nullptr;
            }
        };

        ImGui::BeginTooltip();
        // Quality color for name
        auto qColor = ui::getQualityColor(static_cast<game::ItemQuality>(info->quality));
        ImGui::TextColored(qColor, "%s", info->name.c_str());

        // Heroic indicator (green, matches WoW tooltip style)
        constexpr uint32_t kFlagHeroic         = 0x8;
        constexpr uint32_t kFlagUniqueEquipped = 0x1000000;
        if (info->itemFlags & kFlagHeroic)
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "Heroic");

        // Bind type (appears right under name in WoW)
        switch (info->bindType) {
            case 1: ImGui::TextDisabled("Binds when picked up");   break;
            case 2: ImGui::TextDisabled("Binds when equipped");    break;
            case 3: ImGui::TextDisabled("Binds when used");        break;
            case 4: ImGui::TextDisabled("Quest Item");             break;
        }
        // Unique / Unique-Equipped
        if (info->maxCount == 1)
            ImGui::TextColored(ui::colors::kTooltipGold, "Unique");
        else if (info->itemFlags & kFlagUniqueEquipped)
            ImGui::TextColored(ui::colors::kTooltipGold, "Unique-Equipped");

        // Slot type
        if (info->inventoryType > 0) {
            const char* slotName = ui::getInventorySlotName(info->inventoryType);
            if (slotName[0]) {
                if (!info->subclassName.empty())
                    ImGui::TextColored(ui::colors::kLightGray, "%s  %s", slotName, info->subclassName.c_str());
                else
                    ImGui::TextColored(ui::colors::kLightGray, "%s", slotName);
            }
        }
        auto isWeaponInventoryType = [](uint32_t invType) {
            switch (invType) {
                case 13: // One-Hand
                case 15: // Ranged
                case 17: // Two-Hand
                case 21: // Main Hand
                case 25: // Thrown
                case 26: // Ranged Right
                    return true;
                default:
                    return false;
            }
        };
        const bool isWeapon = isWeaponInventoryType(info->inventoryType);

        // Item level (after slot/subclass)
        if (info->itemLevel > 0)
            ImGui::TextDisabled("Item Level %u", info->itemLevel);

        if (isWeapon && info->damageMax > 0.0f && info->delayMs > 0) {
            float speed = static_cast<float>(info->delayMs) / 1000.0f;
            float dps = ((info->damageMin + info->damageMax) * 0.5f) / speed;
            // WoW-style: "22 - 41 Damage" with speed right-aligned on same row
            char dmgBuf[64], spdBuf[32];
            std::snprintf(dmgBuf, sizeof(dmgBuf), "%d - %d Damage",
                          static_cast<int>(info->damageMin), static_cast<int>(info->damageMax));
            std::snprintf(spdBuf, sizeof(spdBuf), "Speed %.2f", speed);
            float spdW = ImGui::CalcTextSize(spdBuf).x;
            ImGui::Text("%s", dmgBuf);
            ImGui::SameLine(ImGui::GetWindowWidth() - spdW - 16.0f);
            ImGui::Text("%s", spdBuf);
            ImGui::TextDisabled("(%.1f damage per second)", dps);
        }
        ImVec4 green(0.0f, 1.0f, 0.0f, 1.0f);
        auto appendBonus = [](std::string& out, int32_t val, const char* shortName) {
            if (val <= 0) return;
            if (!out.empty()) out += "  ";
            out += "+" + std::to_string(val) + " ";
            out += shortName;
        };
        std::string bonusLine;
        appendBonus(bonusLine, info->strength, "Str");
        appendBonus(bonusLine, info->agility, "Agi");
        appendBonus(bonusLine, info->stamina, "Sta");
        appendBonus(bonusLine, info->intellect, "Int");
        appendBonus(bonusLine, info->spirit, "Spi");
        if (!bonusLine.empty()) {
            ImGui::TextColored(green, "%s", bonusLine.c_str());
        }
        if (info->armor > 0) {
            ImGui::Text("%d Armor", info->armor);
        }
        // Elemental resistances (fire resist gear, nature resist gear, etc.)
        {
            const int32_t resVals[6] = {
                info->holyRes, info->fireRes, info->natureRes,
                info->frostRes, info->shadowRes, info->arcaneRes
            };
            static constexpr const char* resLabels[6] = {
                "Holy Resistance", "Fire Resistance", "Nature Resistance",
                "Frost Resistance", "Shadow Resistance", "Arcane Resistance"
            };
            for (int ri = 0; ri < 6; ++ri)
                if (resVals[ri] > 0) ImGui::Text("+%d %s", resVals[ri], resLabels[ri]);
        }
        // Extra stats (hit/crit/haste/sp/ap/expertise/resilience/etc.)
        if (!info->extraStats.empty()) {
            auto statName = [](uint32_t t) -> const char* {
                switch (t) {
                    case 12: return "Defense Rating";
                    case 13: return "Dodge Rating";
                    case 14: return "Parry Rating";
                    case 15: return "Block Rating";
                    case 16: case 17: case 18: case 31: return "Hit Rating";
                    case 19: case 20: case 21: case 32: return "Critical Strike Rating";
                    case 28: case 29: case 30: case 35: return "Haste Rating";
                    case 34: return "Resilience Rating";
                    case 36: return "Expertise Rating";
                    case 37: return "Attack Power";
                    case 38: return "Ranged Attack Power";
                    case 45: return "Spell Power";
                    case 46: return "Healing Power";
                    case 47: return "Spell Damage";
                    case 49: return "Mana per 5 sec.";
                    case 43: return "Spell Penetration";
                    case 44: return "Block Value";
                    default: return nullptr;
                }
            };
            for (const auto& es : info->extraStats) {
                const char* nm = statName(es.statType);
                if (nm && es.statValue > 0)
                    ImGui::TextColored(green, "+%d %s", es.statValue, nm);
            }
        }
        // Gem sockets (WotLK only — socketColor != 0 means socket present)
        // socketColor bitmask: 1=Meta, 2=Red, 4=Yellow, 8=Blue
        {
            const auto& kSocketTypes = ui::kSocketTypes;
            bool hasSocket = false;
            for (int s = 0; s < 3; ++s) {
                if (info->socketColor[s] == 0) continue;
                if (!hasSocket) { ImGui::Spacing(); hasSocket = true; }
                for (const auto& st : kSocketTypes) {
                    if (info->socketColor[s] & st.mask) {
                        ImGui::TextColored(st.col, "%s", st.label);
                        break;
                    }
                }
            }
            if (hasSocket && info->socketBonus != 0) {
                // Socket bonus ID maps to SpellItemEnchantment.dbc — lazy-load names
                static std::unordered_map<uint32_t, std::string> s_enchantNames;
                static bool s_enchantNamesLoaded = false;
                if (!s_enchantNamesLoaded && assetMgr) {
                    s_enchantNamesLoaded = true;
                    auto dbc = assetMgr->loadDBC("SpellItemEnchantment.dbc");
                    if (dbc && dbc->isLoaded()) {
                        const auto* lay = pipeline::getActiveDBCLayout()
                            ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
                        uint32_t nameField = lay ? lay->field("Name") : 8u;
                        if (nameField == 0xFFFFFFFF) nameField = 8;
                        uint32_t fc = dbc->getFieldCount();
                        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                            uint32_t eid = dbc->getUInt32(r, 0);
                            if (eid == 0 || nameField >= fc) continue;
                            std::string ename = dbc->getString(r, nameField);
                            if (!ename.empty()) s_enchantNames[eid] = std::move(ename);
                        }
                    }
                }
                auto enchIt = s_enchantNames.find(info->socketBonus);
                if (enchIt != s_enchantNames.end())
                    ImGui::TextColored(colors::kSocketGreen, "Socket Bonus: %s", enchIt->second.c_str());
                else
                    ImGui::TextColored(colors::kSocketGreen, "Socket Bonus: (id %u)", info->socketBonus);
            }
        }
        // Item set membership
        if (info->itemSetId != 0) {
            struct SetEntry {
                std::string name;
                std::array<uint32_t, 10> itemIds{};
                std::array<uint32_t, 10> spellIds{};
                std::array<uint32_t, 10> thresholds{};
            };
            static std::unordered_map<uint32_t, SetEntry> s_setData;
            static bool s_setDataLoaded = false;
            if (!s_setDataLoaded && assetMgr) {
                s_setDataLoaded = true;
                auto dbc = assetMgr->loadDBC("ItemSet.dbc");
                if (dbc && dbc->isLoaded()) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("ItemSet") : nullptr;
                    auto lf = [&](const char* k, uint32_t def) -> uint32_t {
                        return layout ? (*layout)[k] : def;
                    };
                    uint32_t idF = lf("ID", 0), nameF = lf("Name", 1);
                    const auto& itemKeys = ui::kItemSetItemKeys;
                    const auto& spellKeys = ui::kItemSetSpellKeys;
                    const auto& thrKeys = ui::kItemSetThresholdKeys;
                    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                        uint32_t id = dbc->getUInt32(r, idF);
                        if (!id) continue;
                        SetEntry e;
                        e.name = dbc->getString(r, nameF);
                        for (int i = 0; i < 10; ++i) {
                            e.itemIds[i]    = dbc->getUInt32(r, layout ? (*layout)[itemKeys[i]]  : uint32_t(18 + i));
                            e.spellIds[i]   = dbc->getUInt32(r, layout ? (*layout)[spellKeys[i]] : uint32_t(28 + i));
                            e.thresholds[i] = dbc->getUInt32(r, layout ? (*layout)[thrKeys[i]]   : uint32_t(38 + i));
                        }
                        s_setData[id] = std::move(e);
                    }
                }
            }
            ImGui::Spacing();
            const auto& inv = gameHandler.getInventory();
            auto setIt = s_setData.find(info->itemSetId);
            if (setIt != s_setData.end()) {
                const SetEntry& se = setIt->second;
                int equipped = 0, total = 0;
                for (int i = 0; i < 10; ++i) {
                    if (se.itemIds[i] == 0) continue;
                    ++total;
                    for (int sl = 0; sl < game::Inventory::NUM_EQUIP_SLOTS; sl++) {
                        const auto& eq = inv.getEquipSlot(static_cast<game::EquipSlot>(sl));
                        if (!eq.empty() && eq.item.itemId == se.itemIds[i]) { ++equipped; break; }
                    }
                }
                if (total > 0)
                    ImGui::TextColored(ui::colors::kTooltipGold,
                        "%s (%d/%d)", se.name.empty() ? "Set" : se.name.c_str(), equipped, total);
                else if (!se.name.empty())
                    ImGui::TextColored(ui::colors::kTooltipGold, "%s", se.name.c_str());
                for (int i = 0; i < 10; ++i) {
                    if (se.spellIds[i] == 0 || se.thresholds[i] == 0) continue;
                    const std::string& bname = gameHandler.getSpellName(se.spellIds[i]);
                    bool active = (equipped >= static_cast<int>(se.thresholds[i]));
                    ImVec4 col = active ? colors::kActiveGreen : colors::kInactiveGray;
                    if (!bname.empty())
                        ImGui::TextColored(col, "(%u) %s", se.thresholds[i], bname.c_str());
                    else
                        ImGui::TextColored(col, "(%u) Set Bonus", se.thresholds[i]);
                }
            } else {
                ImGui::TextColored(ui::colors::kTooltipGold, "Set (id %u)", info->itemSetId);
            }
        }
        // Item spell effects (Use / Equip / Chance on Hit / Teaches)
        for (const auto& sp : info->spells) {
            if (sp.spellId == 0) continue;
            const char* triggerLabel = nullptr;
            switch (sp.spellTrigger) {
                case 0: triggerLabel = "Use";          break;
                case 1: triggerLabel = "Equip";        break;
                case 2: triggerLabel = "Chance on Hit"; break;
                case 5: triggerLabel = "Teaches";      break;
            }
            if (!triggerLabel) continue;
            // Use full spell description if available (matches inventory tooltip style)
            const std::string& spDesc = gameHandler.getSpellDescription(sp.spellId);
            const std::string& spText = !spDesc.empty() ? spDesc
                                        : gameHandler.getSpellName(sp.spellId);
            if (!spText.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f);
                ImGui::TextColored(colors::kCyan,
                                   "%s: %s", triggerLabel, spText.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        // Required level
        if (info->requiredLevel > 1)
            ImGui::TextDisabled("Requires Level %u", info->requiredLevel);
        // Required skill (e.g. "Requires Blacksmithing (300)")
        if (info->requiredSkill != 0 && info->requiredSkillRank > 0) {
            static std::unordered_map<uint32_t, std::string> s_skillNames;
            static bool s_skillNamesLoaded = false;
            if (!s_skillNamesLoaded && assetMgr) {
                s_skillNamesLoaded = true;
                auto dbc = assetMgr->loadDBC("SkillLine.dbc");
                if (dbc && dbc->isLoaded()) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("SkillLine") : nullptr;
                    uint32_t idF   = layout ? (*layout)["ID"]   : 0u;
                    uint32_t nameF = layout ? (*layout)["Name"] : 2u;
                    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                        uint32_t sid = dbc->getUInt32(r, idF);
                        if (!sid) continue;
                        std::string sname = dbc->getString(r, nameF);
                        if (!sname.empty()) s_skillNames[sid] = std::move(sname);
                    }
                }
            }
            uint32_t playerSkillVal = 0;
            const auto& skills = gameHandler.getPlayerSkills();
            auto skPit = skills.find(info->requiredSkill);
            if (skPit != skills.end()) playerSkillVal = skPit->second.effectiveValue();
            bool meetsSkill = (playerSkillVal == 0 || playerSkillVal >= info->requiredSkillRank);
            ImVec4 skColor = meetsSkill ? ImVec4(1.0f, 1.0f, 1.0f, 0.75f) : colors::kPaleRed;
            auto skIt = s_skillNames.find(info->requiredSkill);
            if (skIt != s_skillNames.end())
                ImGui::TextColored(skColor, "Requires %s (%u)", skIt->second.c_str(), info->requiredSkillRank);
            else
                ImGui::TextColored(skColor, "Requires Skill %u (%u)", info->requiredSkill, info->requiredSkillRank);
        }
        // Required reputation (e.g. "Requires Exalted with Argent Dawn")
        if (info->requiredReputationFaction != 0 && info->requiredReputationRank > 0) {
            static std::unordered_map<uint32_t, std::string> s_factionNames;
            static bool s_factionNamesLoaded = false;
            if (!s_factionNamesLoaded && assetMgr) {
                s_factionNamesLoaded = true;
                auto dbc = assetMgr->loadDBC("Faction.dbc");
                if (dbc && dbc->isLoaded()) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("Faction") : nullptr;
                    uint32_t idF   = layout ? (*layout)["ID"]   : 0u;
                    uint32_t nameF = layout ? (*layout)["Name"] : 20u;
                    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                        uint32_t fid = dbc->getUInt32(r, idF);
                        if (!fid) continue;
                        std::string fname = dbc->getString(r, nameF);
                        if (!fname.empty()) s_factionNames[fid] = std::move(fname);
                    }
                }
            }
            static constexpr const char* kRepRankNames[] = {
                "Hated", "Hostile", "Unfriendly", "Neutral",
                "Friendly", "Honored", "Revered", "Exalted"
            };
            const char* rankName = (info->requiredReputationRank < 8)
                ? kRepRankNames[info->requiredReputationRank] : "Unknown";
            auto fIt = s_factionNames.find(info->requiredReputationFaction);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.75f), "Requires %s with %s",
                rankName,
                fIt != s_factionNames.end() ? fIt->second.c_str() : "Unknown Faction");
        }
        // Class restriction (e.g. "Classes: Paladin, Warrior")
        if (info->allowableClass != 0) {
            const auto& kClasses = ui::kClassMasks;
            int matchCount = 0;
            for (const auto& kc : kClasses)
                if (info->allowableClass & kc.mask) ++matchCount;
            if (matchCount > 0 && matchCount < 10) {
                char classBuf[128] = "Classes: ";
                bool first = true;
                for (const auto& kc : kClasses) {
                    if (!(info->allowableClass & kc.mask)) continue;
                    if (!first) strncat(classBuf, ", ", sizeof(classBuf) - strlen(classBuf) - 1);
                    strncat(classBuf, kc.name, sizeof(classBuf) - strlen(classBuf) - 1);
                    first = false;
                }
                uint8_t pc = gameHandler.getPlayerClass();
                uint32_t pmask = (pc > 0 && pc <= 10) ? (1u << (pc - 1)) : 0u;
                bool playerAllowed = (pmask == 0 || (info->allowableClass & pmask));
                ImVec4 clColor = playerAllowed ? ImVec4(1.0f, 1.0f, 1.0f, 0.75f) : colors::kPaleRed;
                ImGui::TextColored(clColor, "%s", classBuf);
            }
        }
        // Race restriction (e.g. "Races: Night Elf, Human")
        if (info->allowableRace != 0) {
            const auto& kRaces = ui::kRaceMasks;
            constexpr uint32_t kAllPlayable = 1|2|4|8|16|32|64|128|512|1024;
            if ((info->allowableRace & kAllPlayable) != kAllPlayable) {
                int matchCount = 0;
                for (const auto& kr : kRaces)
                    if (info->allowableRace & kr.mask) ++matchCount;
                if (matchCount > 0) {
                    char raceBuf[160] = "Races: ";
                    bool first = true;
                    for (const auto& kr : kRaces) {
                        if (!(info->allowableRace & kr.mask)) continue;
                        if (!first) strncat(raceBuf, ", ", sizeof(raceBuf) - strlen(raceBuf) - 1);
                        strncat(raceBuf, kr.name, sizeof(raceBuf) - strlen(raceBuf) - 1);
                        first = false;
                    }
                    uint8_t pr = gameHandler.getPlayerRace();
                    uint32_t pmask = (pr > 0 && pr <= 11) ? (1u << (pr - 1)) : 0u;
                    bool playerAllowed = (pmask == 0 || (info->allowableRace & pmask));
                    ImVec4 rColor = playerAllowed ? ImVec4(1.0f, 1.0f, 1.0f, 0.75f) : colors::kPaleRed;
                    ImGui::TextColored(rColor, "%s", raceBuf);
                }
            }
        }
        // Flavor / lore text (shown in gold italic in WoW, use a yellow-ish dim color here)
        if (!info->description.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(300.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 0.85f), "\"%s\"", info->description.c_str());
            ImGui::PopTextWrapPos();
        }
        if (info->sellPrice > 0) {
            ImGui::TextDisabled("Sell:"); ImGui::SameLine(0, 4);
            renderCoinsFromCopper(info->sellPrice);
        }

        if (ImGui::GetIO().KeyShift && info->inventoryType > 0) {
            if (const auto* eq = findComparableEquipped(static_cast<uint8_t>(info->inventoryType))) {
                ImGui::Separator();
                ImGui::TextDisabled("Equipped:");
                VkDescriptorSet eqIcon = inventoryScreen.getItemIcon(eq->item.displayInfoId);
                if (eqIcon) {
                    ImGui::Image((ImTextureID)(uintptr_t)eqIcon, ImVec2(18.0f, 18.0f));
                    ImGui::SameLine();
                }
                ImGui::TextColored(InventoryScreen::getQualityColor(eq->item.quality), "%s", eq->item.name.c_str());
                if (isWeaponInventoryType(eq->item.inventoryType) &&
                    eq->item.damageMax > 0.0f && eq->item.delayMs > 0) {
                    float speed = static_cast<float>(eq->item.delayMs) / 1000.0f;
                    float dps = ((eq->item.damageMin + eq->item.damageMax) * 0.5f) / speed;
                    char eqDmg[64], eqSpd[32];
                    std::snprintf(eqDmg, sizeof(eqDmg), "%d - %d Damage",
                                  static_cast<int>(eq->item.damageMin), static_cast<int>(eq->item.damageMax));
                    std::snprintf(eqSpd, sizeof(eqSpd), "Speed %.2f", speed);
                    float eqSpdW = ImGui::CalcTextSize(eqSpd).x;
                    ImGui::Text("%s", eqDmg);
                    ImGui::SameLine(ImGui::GetWindowWidth() - eqSpdW - 16.0f);
                    ImGui::Text("%s", eqSpd);
                    ImGui::TextDisabled("(%.1f damage per second)", dps);
                }
                if (eq->item.armor > 0) {
                    ImGui::Text("%d Armor", eq->item.armor);
                }
                std::string eqBonusLine;
                appendBonus(eqBonusLine, eq->item.strength, "Str");
                appendBonus(eqBonusLine, eq->item.agility, "Agi");
                appendBonus(eqBonusLine, eq->item.stamina, "Sta");
                appendBonus(eqBonusLine, eq->item.intellect, "Int");
                appendBonus(eqBonusLine, eq->item.spirit, "Spi");
                if (!eqBonusLine.empty()) {
                    ImGui::TextColored(green, "%s", eqBonusLine.c_str());
                }
                // Extra stats for the equipped item
                for (const auto& es : eq->item.extraStats) {
                    const char* nm = nullptr;
                    switch (es.statType) {
                        case 12: nm = "Defense Rating"; break;
                        case 13: nm = "Dodge Rating"; break;
                        case 14: nm = "Parry Rating"; break;
                        case 16: case 17: case 18: case 31: nm = "Hit Rating"; break;
                        case 19: case 20: case 21: case 32: nm = "Critical Strike Rating"; break;
                        case 28: case 29: case 30: case 35: nm = "Haste Rating"; break;
                        case 34: nm = "Resilience Rating"; break;
                        case 36: nm = "Expertise Rating"; break;
                        case 37: nm = "Attack Power"; break;
                        case 38: nm = "Ranged Attack Power"; break;
                        case 45: nm = "Spell Power"; break;
                        case 46: nm = "Healing Power"; break;
                        case 49: nm = "Mana per 5 sec."; break;
                        default: break;
                    }
                    if (nm && es.statValue > 0)
                        ImGui::TextColored(green, "+%d %s", es.statValue, nm);
                }
            }
        }
        ImGui::EndTooltip();
    };

    // Helper: render text with clickable URLs and WoW item links
    auto renderTextWithLinks = [&](const std::string& text, const ImVec4& color) {
        size_t pos = 0;
        while (pos < text.size()) {
            // Find next special element: URL or WoW link
            size_t urlStart = text.find("https://", pos);

            // Find next WoW link (may be colored with |c prefix or bare |H)
            size_t linkStart = text.find("|c", pos);
            // Also handle bare |H links without color prefix
            size_t bareItem  = text.find("|Hitem:",  pos);
            size_t bareSpell = text.find("|Hspell:", pos);
            size_t bareQuest = text.find("|Hquest:", pos);
            size_t bareLinkStart = std::min({bareItem, bareSpell, bareQuest});

            // Determine which comes first
            size_t nextSpecial = std::min({urlStart, linkStart, bareLinkStart});

            if (nextSpecial == std::string::npos) {
                // No more special elements, render remaining text
                std::string remaining = text.substr(pos);
                if (!remaining.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    ImGui::TextWrapped("%s", remaining.c_str());
                    ImGui::PopStyleColor();
                }
                break;
            }

            // Render plain text before special element
            if (nextSpecial > pos) {
                std::string before = text.substr(pos, nextSpecial - pos);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextWrapped("%s", before.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 0);
            }

            // Handle WoW item link
            if (nextSpecial == linkStart || nextSpecial == bareLinkStart) {
                ImVec4 linkColor = color;
                size_t hStart = std::string::npos;

                if (nextSpecial == linkStart && text.size() > linkStart + 10) {
                    // Parse |cAARRGGBB color
                    linkColor = parseWowColor(text, linkStart);
                    // Find the nearest |H link of any supported type
                    size_t hItem  = text.find("|Hitem:",        linkStart + 10);
                    size_t hSpell = text.find("|Hspell:",       linkStart + 10);
                    size_t hQuest = text.find("|Hquest:",       linkStart + 10);
                    size_t hAch   = text.find("|Hachievement:", linkStart + 10);
                    hStart = std::min({hItem, hSpell, hQuest, hAch});
                } else if (nextSpecial == bareLinkStart) {
                    hStart = bareLinkStart;
                }

                if (hStart != std::string::npos) {
                    // Determine link type
                    const bool isSpellLink = (text.compare(hStart, 8, "|Hspell:") == 0);
                    const bool isQuestLink = (text.compare(hStart, 8, "|Hquest:") == 0);
                    const bool isAchievLink = (text.compare(hStart, 14, "|Hachievement:") == 0);
                    // Default: item link

                    // Parse the first numeric ID after |Htype:
                    size_t idOffset = isSpellLink ? 8 : (isQuestLink ? 8 : (isAchievLink ? 14 : 7));
                    size_t entryStart = hStart + idOffset;
                    size_t entryEnd = text.find(':', entryStart);
                    uint32_t linkId = 0;
                    if (entryEnd != std::string::npos) {
                        linkId = static_cast<uint32_t>(strtoul(
                            text.substr(entryStart, entryEnd - entryStart).c_str(), nullptr, 10));
                    }

                    // Find display name: |h[Name]|h
                    size_t nameTagStart = text.find("|h[", hStart);
                    size_t nameTagEnd = (nameTagStart != std::string::npos)
                        ? text.find("]|h", nameTagStart + 3) : std::string::npos;

                    std::string linkName = isSpellLink ? "Unknown Spell"
                                        : isQuestLink  ? "Unknown Quest"
                                        : isAchievLink ? "Unknown Achievement"
                                        : "Unknown Item";
                    if (nameTagStart != std::string::npos && nameTagEnd != std::string::npos) {
                        linkName = text.substr(nameTagStart + 3, nameTagEnd - nameTagStart - 3);
                    }

                    // Find end of entire link sequence (|r or after ]|h)
                    size_t linkEnd = (nameTagEnd != std::string::npos) ? nameTagEnd + 3 : hStart + idOffset;
                    size_t resetPos = text.find("|r", linkEnd);
                    if (resetPos != std::string::npos && resetPos <= linkEnd + 2) {
                        linkEnd = resetPos + 2;
                    }

                    if (!isSpellLink && !isQuestLink && !isAchievLink) {
                        // --- Item link ---
                        uint32_t itemEntry = linkId;
                        if (itemEntry > 0) {
                            gameHandler.ensureItemInfo(itemEntry);
                        }

                        // Show small icon before item link if available
                        if (itemEntry > 0) {
                            const auto* chatInfo = gameHandler.getItemInfo(itemEntry);
                            if (chatInfo && chatInfo->valid && chatInfo->displayInfoId != 0) {
                                VkDescriptorSet chatIcon = inventoryScreen.getItemIcon(chatInfo->displayInfoId);
                                if (chatIcon) {
                                    ImGui::Image((ImTextureID)(uintptr_t)chatIcon, ImVec2(12, 12));
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                        renderItemLinkTooltip(itemEntry);
                                    }
                                    ImGui::SameLine(0, 2);
                                }
                            }
                        }

                        // Render bracketed item name in quality color
                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, linkColor);
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if (itemEntry > 0) {
                                renderItemLinkTooltip(itemEntry);
                            }
                        }
                    } else if (isSpellLink) {
                        // --- Spell link: |Hspell:SPELLID:RANK|h[Name]|h ---
                        // Small icon (use spell icon cache if available)
                        VkDescriptorSet spellIcon = (linkId > 0) ? getSpellIcon(linkId, assetMgr) : VK_NULL_HANDLE;
                        if (spellIcon) {
                            ImGui::Image((ImTextureID)(uintptr_t)spellIcon, ImVec2(12, 12));
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                spellbookScreen.renderSpellInfoTooltip(linkId, gameHandler, assetMgr);
                            }
                            ImGui::SameLine(0, 2);
                        }

                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, linkColor);
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if (linkId > 0) {
                                spellbookScreen.renderSpellInfoTooltip(linkId, gameHandler, assetMgr);
                            }
                        }
                    } else if (isQuestLink) {
                        // --- Quest link: |Hquest:QUESTID:QUESTLEVEL|h[Name]|h ---
                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, colors::kWarmGold); // gold
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::BeginTooltip();
                            ImGui::TextColored(colors::kWarmGold, "%s", linkName.c_str());
                            // Parse quest level (second field after questId)
                            if (entryEnd != std::string::npos) {
                                size_t lvlEnd = text.find(':', entryEnd + 1);
                                if (lvlEnd == std::string::npos) lvlEnd = text.find('|', entryEnd + 1);
                                if (lvlEnd != std::string::npos) {
                                    uint32_t qLvl = static_cast<uint32_t>(strtoul(
                                        text.substr(entryEnd + 1, lvlEnd - entryEnd - 1).c_str(), nullptr, 10));
                                    if (qLvl > 0) ImGui::TextDisabled("Level %u Quest", qLvl);
                                }
                            }
                            ImGui::TextDisabled("Click quest log to view details");
                            ImGui::EndTooltip();
                        }
                        // Click: open quest log and select this quest if we have it
                        if (ImGui::IsItemClicked() && linkId > 0) {
                            questLogScreen.openAndSelectQuest(linkId);
                        }
                    } else {
                        // --- Achievement link ---
                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, colors::kBrightGold); // gold
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::SetTooltip("Achievement: %s", linkName.c_str());
                        }
                    }

                    // Shift-click: insert entire link back into chat input
                    if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift) {
                        std::string linkText = text.substr(nextSpecial, linkEnd - nextSpecial);
                        size_t curLen = strlen(chatInputBuffer_);
                        if (curLen + linkText.size() + 1 < sizeof(chatInputBuffer_)) {
                            strncat(chatInputBuffer_, linkText.c_str(), sizeof(chatInputBuffer_) - curLen - 1);
                            chatInputMoveCursorToEnd_ = true;
                        }
                    }

                    pos = linkEnd;
                    continue;
                }

                // Not an item link — treat as colored text: |cAARRGGBB...text...|r
                if (nextSpecial == linkStart && text.size() > linkStart + 10) {
                    ImVec4 cColor = parseWowColor(text, linkStart);
                    size_t textStart = linkStart + 10; // after |cAARRGGBB
                    size_t resetPos2 = text.find("|r", textStart);
                    std::string coloredText;
                    if (resetPos2 != std::string::npos) {
                        coloredText = text.substr(textStart, resetPos2 - textStart);
                        pos = resetPos2 + 2; // skip |r
                    } else {
                        coloredText = text.substr(textStart);
                        pos = text.size();
                    }
                    // Strip any remaining WoW markup from the colored segment
                    // (e.g. |H...|h pairs that aren't item links)
                    std::string clean;
                    for (size_t i = 0; i < coloredText.size(); i++) {
                        if (coloredText[i] == '|' && i + 1 < coloredText.size()) {
                            char next = coloredText[i + 1];
                            if (next == 'H') {
                                // Skip |H...|h
                                size_t hEnd = coloredText.find("|h", i + 2);
                                if (hEnd != std::string::npos) { i = hEnd + 1; continue; }
                            } else if (next == 'h') {
                                i += 1; continue; // skip |h
                            } else if (next == 'r') {
                                i += 1; continue; // skip |r
                            }
                        }
                        clean += coloredText[i];
                    }
                    if (!clean.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, cColor);
                        ImGui::TextWrapped("%s", clean.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine(0, 0);
                    }
                } else {
                    // Bare |c without enough chars for color — render literally
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    ImGui::TextWrapped("|c");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 0);
                    pos = nextSpecial + 2;
                }
                continue;
            }

            // Handle URL
            if (nextSpecial == urlStart) {
                size_t urlEnd = text.find_first_of(" \t\n\r", urlStart);
                if (urlEnd == std::string::npos) urlEnd = text.size();
                std::string url = text.substr(urlStart, urlEnd - urlStart);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                ImGui::TextWrapped("%s", url.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Open: %s", url.c_str());
                }
                if (ImGui::IsItemClicked()) {
                    std::string cmd = "xdg-open '" + url + "' &";
                    [[maybe_unused]] int result = system(cmd.c_str());
                }
                ImGui::PopStyleColor();

                pos = urlEnd;
                continue;
            }
        }
    };

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
        if (!shouldShowMessage(msg, activeChatTab)) continue;
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

        ImVec4 color = getChatTypeColor(msg.type);

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
                fullMsg = tsPrefix + "[" + std::string(getChatTypeName(msg.type)) + "] " + tagPrefix + resolvedSenderName + ": " + processedMessage;
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
                fullMsg = tsPrefix + "[" + std::string(getChatTypeName(msg.type)) + "] " + processedMessage;
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
        renderTextWithLinks(fullMsg, isMention ? ImVec4(1.0f, 0.9f, 0.35f, 1.0f) : color);
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

        // Tab: slash-command autocomplete
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            if (data->BufTextLen > 0 && data->Buf[0] == '/') {
                // Split buffer into command word and trailing args
                std::string fullBuf(data->Buf, data->BufTextLen);
                size_t spacePos = fullBuf.find(' ');
                std::string word = (spacePos != std::string::npos) ? fullBuf.substr(0, spacePos) : fullBuf;
                std::string rest = (spacePos != std::string::npos) ? fullBuf.substr(spacePos) : "";

                // Normalize to lowercase for matching
                std::string lowerWord = word;
                for (auto& ch : lowerWord) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

                static const std::vector<std::string> kCmds = {
                    "/afk", "/assist", "/away",
                    "/cancelaura", "/cancelform", "/cancellogout", "/cancelshapeshift",
                    "/cast", "/castsequence", "/chathelp", "/clear", "/clearfocus",
                    "/clearmainassist", "/clearmaintank", "/cleartarget", "/cloak",
                    "/combatlog", "/dance", "/difficulty", "/dismount", "/dnd", "/do", "/duel", "/dump",
                    "/e", "/emote", "/equip", "/equipset", "/exit",
                    "/focus", "/follow", "/forfeit", "/friend",
                    "/g", "/gdemote", "/ginvite", "/gkick", "/gleader", "/gmotd",
                    "/gmticket", "/gpromote", "/gquit", "/grouploot", "/groster",
                    "/guild", "/guildinfo",
                    "/helm", "/help",
                    "/i", "/ignore", "/inspect", "/instance", "/invite",
                    "/j", "/join", "/kick", "/kneel",
                    "/l", "/leave", "/leaveparty", "/loc", "/local", "/logout",
                    "/lootmethod", "/lootthreshold",
                    "/macrohelp", "/mainassist", "/maintank", "/mark", "/me",
                    "/notready",
                    "/p", "/party", "/petaggressive", "/petattack", "/petdefensive",
                    "/petdismiss", "/petfollow", "/pethalt", "/petpassive", "/petstay",
                    "/played", "/pvp",
                    "/quit",
                    "/r", "/raid", "/raidconvert", "/raidinfo", "/raidwarning", "/random", "/ready",
                    "/readycheck", "/reload", "/reloadui", "/removefriend",
                    "/reply", "/rl", "/roll", "/run",
                    "/s", "/say", "/score", "/screenshot", "/script", "/setloot",
                    "/shout", "/sit", "/stand",
                    "/startattack", "/stopattack", "/stopcasting", "/stopfollow", "/stopmacro",
                    "/t", "/target", "/targetenemy", "/targetfriend", "/targetlast",
                    "/threat", "/ticket", "/time", "/trade",
                    "/unignore", "/uninvite", "/unstuck", "/use",
                    "/w", "/whisper", "/who", "/wts", "/wtb",
                    "/y", "/yell", "/zone"
                };

                // New session if prefix changed
                if (self->chatTabMatchIdx_ < 0 || self->chatTabPrefix_ != lowerWord) {
                    self->chatTabPrefix_ = lowerWord;
                    self->chatTabMatches_.clear();
                    for (const auto& cmd : kCmds) {
                        if (cmd.size() >= lowerWord.size() &&
                            cmd.compare(0, lowerWord.size(), lowerWord) == 0)
                            self->chatTabMatches_.push_back(cmd);
                    }
                    self->chatTabMatchIdx_ = 0;
                } else {
                    // Cycle forward through matches
                    ++self->chatTabMatchIdx_;
                    if (self->chatTabMatchIdx_ >= static_cast<int>(self->chatTabMatches_.size()))
                        self->chatTabMatchIdx_ = 0;
                }

                if (!self->chatTabMatches_.empty()) {
                    std::string match = self->chatTabMatches_[self->chatTabMatchIdx_];
                    // Append trailing space when match is unambiguous
                    if (self->chatTabMatches_.size() == 1 && rest.empty())
                        match += ' ';
                    std::string newBuf = match + rest;
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, newBuf.c_str());
                }
            } else if (data->BufTextLen > 0) {
                // Player name tab-completion for commands like /w, /whisper, /invite, /trade, /duel
                // Also works for plain text (completes nearby player names)
                std::string fullBuf(data->Buf, data->BufTextLen);
                size_t spacePos = fullBuf.find(' ');
                bool isNameCommand = false;
                std::string namePrefix;
                size_t replaceStart = 0;

                if (fullBuf[0] == '/' && spacePos != std::string::npos) {
                    std::string cmd = fullBuf.substr(0, spacePos);
                    for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    // Commands that take a player name as the first argument after the command
                    if (cmd == "/w" || cmd == "/whisper" || cmd == "/invite" ||
                        cmd == "/trade" || cmd == "/duel" || cmd == "/follow" ||
                        cmd == "/inspect" || cmd == "/friend" || cmd == "/removefriend" ||
                        cmd == "/ignore" || cmd == "/unignore" || cmd == "/who" ||
                        cmd == "/t" || cmd == "/target" || cmd == "/kick" ||
                        cmd == "/uninvite" || cmd == "/ginvite" || cmd == "/gkick") {
                        // Extract the partial name after the space
                        namePrefix = fullBuf.substr(spacePos + 1);
                        // Only complete the first word after the command
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

                    if (self->chatTabMatchIdx_ < 0 || self->chatTabPrefix_ != lowerPrefix) {
                        self->chatTabPrefix_ = lowerPrefix;
                        self->chatTabMatches_.clear();
                        // Search player name cache and nearby entities
                        auto* gh = self->cachedGameHandler_;
                        // Party/raid members
                        for (const auto& m : gh->getPartyData().members) {
                            if (m.name.empty()) continue;
                            std::string lname = m.name;
                            for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0)
                                self->chatTabMatches_.push_back(m.name);
                        }
                        // Friends
                        for (const auto& c : gh->getContacts()) {
                            if (!c.isFriend() || c.name.empty()) continue;
                            std::string lname = c.name;
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                // Avoid duplicates from party
                                bool dup = false;
                                for (const auto& em : self->chatTabMatches_)
                                    if (em == c.name) { dup = true; break; }
                                if (!dup) self->chatTabMatches_.push_back(c.name);
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
                                for (const auto& em : self->chatTabMatches_)
                                    if (em == player->getName()) { dup = true; break; }
                                if (!dup) self->chatTabMatches_.push_back(player->getName());
                            }
                        }
                        // Last whisper sender
                        if (!gh->getLastWhisperSender().empty()) {
                            std::string lname = gh->getLastWhisperSender();
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                bool dup = false;
                                for (const auto& em : self->chatTabMatches_)
                                    if (em == gh->getLastWhisperSender()) { dup = true; break; }
                                if (!dup) self->chatTabMatches_.insert(self->chatTabMatches_.begin(), gh->getLastWhisperSender());
                            }
                        }
                        self->chatTabMatchIdx_ = 0;
                    } else {
                        ++self->chatTabMatchIdx_;
                        if (self->chatTabMatchIdx_ >= static_cast<int>(self->chatTabMatches_.size()))
                            self->chatTabMatchIdx_ = 0;
                    }

                    if (!self->chatTabMatches_.empty()) {
                        std::string match = self->chatTabMatches_[self->chatTabMatchIdx_];
                        std::string prefix = fullBuf.substr(0, replaceStart);
                        std::string newBuf = prefix + match;
                        if (self->chatTabMatches_.size() == 1) newBuf += ' ';
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
            self->chatTabMatchIdx_ = -1;
            self->chatTabMatches_.clear();

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
        sendChatMessage(gameHandler, inventoryScreen, spellbookScreen, questLogScreen);
        // Close chat input on send so movement keys work immediately.
        refocusChatInput_ = false;
        ImGui::ClearActiveID();
    }
    ImGui::PopStyleColor();

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


// Collect all non-comment, non-empty lines from a macro body.

} // namespace ui
} // namespace wowee
