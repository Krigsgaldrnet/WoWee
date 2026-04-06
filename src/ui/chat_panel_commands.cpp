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
    using namespace wowee::ui::colors;

    std::string trim(const std::string& s) {
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

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
// WoW macro conditional evaluator
// Parses:  [cond1,cond2] Spell1; [cond3] Spell2; DefaultSpell
// Returns the first matching alternative's argument, or "" if none matches.
// targetOverride is set to a specific GUID if [target=X] was in the conditions,
// or left as UINT64_MAX to mean "use the normal target".
// ---------------------------------------------------------------------------
static std::string evaluateMacroConditionals(const std::string& rawArg,
                                              game::GameHandler& gameHandler,
                                              uint64_t& targetOverride) {
    targetOverride = static_cast<uint64_t>(-1);

    auto& input = core::Input::getInstance();

    const bool shiftHeld = input.isKeyPressed(SDL_SCANCODE_LSHIFT) ||
                           input.isKeyPressed(SDL_SCANCODE_RSHIFT);
    const bool ctrlHeld  = input.isKeyPressed(SDL_SCANCODE_LCTRL)  ||
                           input.isKeyPressed(SDL_SCANCODE_RCTRL);
    const bool altHeld   = input.isKeyPressed(SDL_SCANCODE_LALT)   ||
                           input.isKeyPressed(SDL_SCANCODE_RALT);
    const bool anyMod    = shiftHeld || ctrlHeld || altHeld;

    // Split rawArg on ';' → alternatives
    std::vector<std::string> alts;
    {
        std::string cur;
        for (char c : rawArg) {
            if (c == ';') { alts.push_back(cur); cur.clear(); }
            else            cur += c;
        }
        alts.push_back(cur);
    }

    // Evaluate a single comma-separated condition token.
    // tgt is updated if a target= or @ specifier is found.
    auto evalCond = [&](const std::string& raw, uint64_t& tgt) -> bool {
        std::string c = raw;
        // trim
        size_t s = c.find_first_not_of(" \t"); if (s) c = (s != std::string::npos) ? c.substr(s) : "";
        size_t e = c.find_last_not_of(" \t");  if (e != std::string::npos) c.resize(e + 1);
        if (c.empty()) return true;

        // @target specifiers: @player, @focus, @pet, @mouseover, @target
        if (!c.empty() && c[0] == '@') {
            std::string spec = c.substr(1);
            if (spec == "player")          tgt = gameHandler.getPlayerGuid();
            else if (spec == "focus")      tgt = gameHandler.getFocusGuid();
            else if (spec == "target")     tgt = gameHandler.getTargetGuid();
            else if (spec == "pet") {
                uint64_t pg = gameHandler.getPetGuid();
                if (pg != 0) tgt = pg;
                else return false;  // no pet — skip this alternative
            }
            else if (spec == "mouseover") {
                uint64_t mo = gameHandler.getMouseoverGuid();
                if (mo != 0) tgt = mo;
                else return false;  // no mouseover — skip this alternative
            }
            return true;
        }
        // target=X specifiers
        if (c.rfind("target=", 0) == 0) {
            std::string spec = c.substr(7);
            if (spec == "player")          tgt = gameHandler.getPlayerGuid();
            else if (spec == "focus")      tgt = gameHandler.getFocusGuid();
            else if (spec == "target")     tgt = gameHandler.getTargetGuid();
            else if (spec == "pet") {
                uint64_t pg = gameHandler.getPetGuid();
                if (pg != 0) tgt = pg;
                else return false;  // no pet — skip this alternative
            }
            else if (spec == "mouseover") {
                uint64_t mo = gameHandler.getMouseoverGuid();
                if (mo != 0) tgt = mo;
                else return false;  // no mouseover — skip this alternative
            }
            return true;
        }

        // mod / nomod
        if (c == "nomod" || c == "mod:none") return !anyMod;
        if (c.rfind("mod:", 0) == 0) {
            std::string mods = c.substr(4);
            bool ok = true;
            if (mods.find("shift") != std::string::npos && !shiftHeld) ok = false;
            if (mods.find("ctrl")  != std::string::npos && !ctrlHeld)  ok = false;
            if (mods.find("alt")   != std::string::npos && !altHeld)   ok = false;
            return ok;
        }

        // combat / nocombat
        if (c == "combat")   return gameHandler.isInCombat();
        if (c == "nocombat") return !gameHandler.isInCombat();

        // Helper to get the effective target entity
        auto effTarget = [&]() -> std::shared_ptr<game::Entity> {
            if (tgt != static_cast<uint64_t>(-1) && tgt != 0)
                return gameHandler.getEntityManager().getEntity(tgt);
            return gameHandler.getTarget();
        };

        // exists / noexists
        if (c == "exists")   return effTarget() != nullptr;
        if (c == "noexists") return effTarget() == nullptr;

        // dead / nodead
        if (c == "dead")   {
            auto t = effTarget();
            auto u = t ? std::dynamic_pointer_cast<game::Unit>(t) : nullptr;
            return u && u->getHealth() == 0;
        }
        if (c == "nodead") {
            auto t = effTarget();
            auto u = t ? std::dynamic_pointer_cast<game::Unit>(t) : nullptr;
            return u && u->getHealth() > 0;
        }

        // help (friendly) / harm (hostile) and their no- variants
        auto unitHostile = [&](const std::shared_ptr<game::Entity>& t) -> bool {
            if (!t) return false;
            auto u = std::dynamic_pointer_cast<game::Unit>(t);
            return u && gameHandler.isHostileFactionPublic(u->getFactionTemplate());
        };
        if (c == "harm" || c == "nohelp") { return unitHostile(effTarget()); }
        if (c == "help" || c == "noharm") { return !unitHostile(effTarget()); }

        // mounted / nomounted
        if (c == "mounted")   return gameHandler.isMounted();
        if (c == "nomounted") return !gameHandler.isMounted();

        // swimming / noswimming
        if (c == "swimming")   return gameHandler.isSwimming();
        if (c == "noswimming") return !gameHandler.isSwimming();

        // flying / noflying (CAN_FLY + FLYING flags active)
        if (c == "flying")   return gameHandler.isPlayerFlying();
        if (c == "noflying") return !gameHandler.isPlayerFlying();

        // channeling / nochanneling
        if (c == "channeling")   return gameHandler.isCasting() && gameHandler.isChanneling();
        if (c == "nochanneling") return !(gameHandler.isCasting() && gameHandler.isChanneling());

        // stealthed / nostealthed (unit flag 0x02000000 = UNIT_FLAG_SNEAKING)
        auto isStealthedFn = [&]() -> bool {
            auto pe = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
            if (!pe) return false;
            auto pu = std::dynamic_pointer_cast<game::Unit>(pe);
            return pu && (pu->getUnitFlags() & 0x02000000u) != 0;
        };
        if (c == "stealthed")   return isStealthedFn();
        if (c == "nostealthed") return !isStealthedFn();

        // pet / nopet — player has an active pet (hunters, warlocks, DKs)
        if (c == "pet")   return gameHandler.hasPet();
        if (c == "nopet") return !gameHandler.hasPet();

        // indoors / outdoors — WMO interior detection (affects mount type selection)
        if (c == "indoors" || c == "nooutdoors") {
            auto* r = core::Application::getInstance().getRenderer();
            return r && r->isPlayerIndoors();
        }
        if (c == "outdoors" || c == "noindoors") {
            auto* r = core::Application::getInstance().getRenderer();
            return !r || !r->isPlayerIndoors();
        }

        // group / nogroup — player is in a party or raid
        if (c == "group" || c == "party") return gameHandler.isInGroup();
        if (c == "nogroup")               return !gameHandler.isInGroup();

        // raid / noraid — player is in a raid group (groupType == 1)
        if (c == "raid") return gameHandler.isInGroup() && gameHandler.getPartyData().groupType == 1;
        if (c == "noraid") return !gameHandler.isInGroup() || gameHandler.getPartyData().groupType != 1;

        // spec:N — active talent spec (1-based: spec:1 = primary, spec:2 = secondary)
        if (c.rfind("spec:", 0) == 0) {
            uint8_t wantSpec = 0;
            try { wantSpec = static_cast<uint8_t>(std::stoul(c.substr(5))); } catch (...) {}
            return wantSpec > 0 && gameHandler.getActiveTalentSpec() == (wantSpec - 1);
        }

        // noform / nostance — player is NOT in a shapeshift/stance
        if (c == "noform" || c == "nostance") {
            for (const auto& a : gameHandler.getPlayerAuras())
                if (!a.isEmpty() && a.maxDurationMs == -1) return false;
            return true;
        }
        // form:0 same as noform
        if (c == "form:0" || c == "stance:0") {
            for (const auto& a : gameHandler.getPlayerAuras())
                if (!a.isEmpty() && a.maxDurationMs == -1) return false;
            return true;
        }

        // buff:SpellName / nobuff:SpellName — check if the effective target (or player
        // if no target specified) has a buff with the given name.
        // debuff:SpellName / nodebuff:SpellName — same for debuffs (harmful auras).
        auto checkAuraByName = [&](const std::string& spellName, bool wantDebuff,
                                   bool negate) -> bool {
            // Determine which aura list to check: effective target or player
            const std::vector<game::AuraSlot>* auras = nullptr;
            if (tgt != static_cast<uint64_t>(-1) && tgt != 0 && tgt != gameHandler.getPlayerGuid()) {
                // Check target's auras
                auras = &gameHandler.getTargetAuras();
            } else {
                auras = &gameHandler.getPlayerAuras();
            }
            std::string nameLow = spellName;
            for (char& ch : nameLow) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (const auto& a : *auras) {
                if (a.isEmpty() || a.spellId == 0) continue;
                // Filter: debuffs have the HARMFUL flag (0x80) or spell has a dispel type
                bool isDebuff = (a.flags & 0x80) != 0;
                if (wantDebuff ? !isDebuff : isDebuff) continue;
                std::string sn = gameHandler.getSpellName(a.spellId);
                for (char& ch : sn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (sn == nameLow) return !negate;
            }
            return negate;
        };
        if (c.rfind("buff:", 0) == 0 && c.size() > 5)
            return checkAuraByName(c.substr(5), false, false);
        if (c.rfind("nobuff:", 0) == 0 && c.size() > 7)
            return checkAuraByName(c.substr(7), false, true);
        if (c.rfind("debuff:", 0) == 0 && c.size() > 7)
            return checkAuraByName(c.substr(7), true, false);
        if (c.rfind("nodebuff:", 0) == 0 && c.size() > 9)
            return checkAuraByName(c.substr(9), true, true);

        // mounted / nomounted
        if (c == "mounted")   return gameHandler.isMounted();
        if (c == "nomounted") return !gameHandler.isMounted();

        // group (any group) / nogroup / raid
        if (c == "group")  return !gameHandler.getPartyData().isEmpty();
        if (c == "nogroup") return gameHandler.getPartyData().isEmpty();
        if (c == "raid")   {
            const auto& pd = gameHandler.getPartyData();
            return pd.groupType >= 1;  // groupType 1 = raid, 0 = party
        }

        // channeling:SpellName — player is currently channeling that spell
        if (c.rfind("channeling:", 0) == 0 && c.size() > 11) {
            if (!gameHandler.isChanneling()) return false;
            std::string want = c.substr(11);
            for (char& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            uint32_t castSpellId = gameHandler.getCurrentCastSpellId();
            std::string sn = gameHandler.getSpellName(castSpellId);
            for (char& ch : sn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return sn == want;
        }
        if (c == "channeling") return gameHandler.isChanneling();
        if (c == "nochanneling") return !gameHandler.isChanneling();

        // casting (any active cast or channel)
        if (c == "casting")   return gameHandler.isCasting();
        if (c == "nocasting") return !gameHandler.isCasting();

        // vehicle / novehicle (WotLK)
        if (c == "vehicle")   return gameHandler.getVehicleId() != 0;
        if (c == "novehicle") return gameHandler.getVehicleId() == 0;

        // Unknown → permissive (don't block)
        return true;
    };

    for (auto& alt : alts) {
        // trim
        size_t fs = alt.find_first_not_of(" \t");
        if (fs == std::string::npos) continue;
        alt = alt.substr(fs);
        size_t ls = alt.find_last_not_of(" \t");
        if (ls != std::string::npos) alt.resize(ls + 1);

        if (!alt.empty() && alt[0] == '[') {
            size_t close = alt.find(']');
            if (close == std::string::npos) continue;
            std::string condStr  = alt.substr(1, close - 1);
            std::string argPart  = alt.substr(close + 1);
            // Trim argPart
            size_t as = argPart.find_first_not_of(" \t");
            argPart = (as != std::string::npos) ? argPart.substr(as) : "";

            // Evaluate comma-separated conditions
            uint64_t tgt = static_cast<uint64_t>(-1);
            bool pass = true;
            size_t cp = 0;
            while (pass) {
                size_t comma = condStr.find(',', cp);
                std::string tok = condStr.substr(cp, comma == std::string::npos ? std::string::npos : comma - cp);
                if (!evalCond(tok, tgt)) { pass = false; break; }
                if (comma == std::string::npos) break;
                cp = comma + 1;
            }
            if (pass) {
                if (tgt != static_cast<uint64_t>(-1)) targetOverride = tgt;
                return argPart;
            }
        } else {
            // No condition block — default fallback always matches
            return alt;
        }
    }
    return {};
}

// Execute all non-comment lines of a macro body in sequence.
// In WoW, every line executes per click; the server enforces spell-cast limits.
// /stopmacro (with optional conditionals) halts the remaining commands early.

void ChatPanel::executeMacroText(game::GameHandler& gameHandler,
                                  InventoryScreen& inventoryScreen,
                                  SpellbookScreen& spellbookScreen,
                                  QuestLogScreen& questLogScreen,
                                  const std::string& macroText) {
    macroStopped_ = false;
    for (const auto& cmd : allMacroCommands(macroText)) {
        strncpy(chatInputBuffer_, cmd.c_str(), sizeof(chatInputBuffer_) - 1);
        chatInputBuffer_[sizeof(chatInputBuffer_) - 1] = '\0';
        sendChatMessage(gameHandler, inventoryScreen, spellbookScreen, questLogScreen);
        if (macroStopped_) break;
    }
    macroStopped_ = false;
}

// /castsequence persistent state — shared across all macros using the same spell list.
// Keyed by the normalized (lowercase, comma-joined) spell sequence string.
namespace {
struct CastSeqState {
    size_t   index = 0;
    float    lastPressSec = 0.0f;
    uint64_t lastTargetGuid = 0;
    bool     lastInCombat = false;
};
std::unordered_map<std::string, CastSeqState> s_castSeqStates;
}  // namespace


void ChatPanel::sendChatMessage(game::GameHandler& gameHandler,
                                 InventoryScreen& /*inventoryScreen*/,
                                 SpellbookScreen& /*spellbookScreen*/,
                                 QuestLogScreen& /*questLogScreen*/) {
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

        // Track if a channel shortcut should change the chat type dropdown
        int switchChatType = -1;

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

            // Special commands
            if (cmdLower == "logout") {
                core::Application::getInstance().logoutToLogin();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clear") {
                gameHandler.clearChatHistory();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /reload or /reloadui — reload all addons (save variables, re-init Lua, re-scan .toc files)
            if (cmdLower == "reload" || cmdLower == "reloadui" || cmdLower == "rl") {
                auto* am = services_.addonManager;
                if (am) {
                    am->reload();
                    am->fireEvent("VARIABLES_LOADED");
                    am->fireEvent("PLAYER_LOGIN");
                    am->fireEvent("PLAYER_ENTERING_WORLD");
                    game::MessageChatData rlMsg;
                    rlMsg.type = game::ChatType::SYSTEM;
                    rlMsg.language = game::ChatLanguage::UNIVERSAL;
                    rlMsg.message = "Interface reloaded.";
                    gameHandler.addLocalChatMessage(rlMsg);
                } else {
                    game::MessageChatData rlMsg;
                    rlMsg.type = game::ChatType::SYSTEM;
                    rlMsg.language = game::ChatLanguage::UNIVERSAL;
                    rlMsg.message = "Addon system not available.";
                    gameHandler.addLocalChatMessage(rlMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /stopmacro [conditions]
            // Halts execution of the current macro (remaining lines are skipped).
            // With a condition block, only stops if the conditions evaluate to true.
            //   /stopmacro            → always stops
            //   /stopmacro [combat]   → stops only while in combat
            //   /stopmacro [nocombat] → stops only when not in combat
            if (cmdLower == "stopmacro") {
                bool shouldStop = true;
                if (spacePos != std::string::npos) {
                    std::string condArg = command.substr(spacePos + 1);
                    while (!condArg.empty() && condArg.front() == ' ') condArg.erase(condArg.begin());
                    if (!condArg.empty() && condArg.front() == '[') {
                        // Append a sentinel action so evaluateMacroConditionals can signal a match.
                        uint64_t tgtOver = static_cast<uint64_t>(-1);
                        std::string hit = evaluateMacroConditionals(condArg + " __stop__", gameHandler, tgtOver);
                        shouldStop = !hit.empty();
                    }
                }
                if (shouldStop) macroStopped_ = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /invite command
            if (cmdLower == "invite" && spacePos != std::string::npos) {
                std::string targetName = command.substr(spacePos + 1);
                gameHandler.inviteToGroup(targetName);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /inspect command
            if (cmdLower == "inspect") {
                gameHandler.inspectTarget();
                slashCmds_.showInspect = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /threat command
            if (cmdLower == "threat") {
                slashCmds_.toggleThreat = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /score command — BG scoreboard
            if (cmdLower == "score") {
                gameHandler.requestPvpLog();
                slashCmds_.showBgScore = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /time command
            if (cmdLower == "time") {
                gameHandler.queryServerTime();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /loc command — print player coordinates and zone name
            if (cmdLower == "loc" || cmdLower == "coords" || cmdLower == "whereami") {
                const auto& pmi = gameHandler.getMovementInfo();
                std::string zoneName;
                if (auto* rend = services_.renderer)
                    zoneName = rend->getCurrentZoneName();
                char buf[256];
                snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f%s%s",
                         pmi.x, pmi.y, pmi.z,
                         zoneName.empty() ? "" : " — ",
                         zoneName.c_str());
                game::MessageChatData sysMsg;
                sysMsg.type = game::ChatType::SYSTEM;
                sysMsg.language = game::ChatLanguage::UNIVERSAL;
                sysMsg.message = buf;
                gameHandler.addLocalChatMessage(sysMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /screenshot command — capture current frame to PNG
            if (cmdLower == "screenshot" || cmdLower == "ss") {
                slashCmds_.takeScreenshot = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /zone command — print current zone name
            if (cmdLower == "zone") {
                std::string zoneName;
                if (auto* rend = services_.renderer)
                    zoneName = rend->getCurrentZoneName();
                game::MessageChatData sysMsg;
                sysMsg.type = game::ChatType::SYSTEM;
                sysMsg.language = game::ChatLanguage::UNIVERSAL;
                sysMsg.message = zoneName.empty() ? "You are not in a known zone." : "You are in: " + zoneName;
                gameHandler.addLocalChatMessage(sysMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /played command
            if (cmdLower == "played") {
                gameHandler.requestPlayedTime();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ticket command — open GM ticket window
            if (cmdLower == "ticket" || cmdLower == "gmticket" || cmdLower == "gm") {
                slashCmds_.showGmTicket = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /chathelp command — list chat-channel slash commands
            if (cmdLower == "chathelp") {
                static constexpr const char* kChatHelp[] = {
                    "--- Chat Channel Commands ---",
                    "/s [msg]          Say to nearby players",
                    "/y [msg]          Yell to a wider area",
                    "/w <name> [msg]   Whisper to player",
                    "/r [msg]          Reply to last whisper",
                    "/p [msg]          Party chat",
                    "/g [msg]          Guild chat",
                    "/o [msg]          Guild officer chat",
                    "/raid [msg]       Raid chat",
                    "/rw [msg]         Raid warning",
                    "/bg [msg]         Battleground chat",
                    "/1 [msg]          General channel",
                    "/2 [msg]          Trade channel  (also /wts /wtb)",
                    "/<N> [msg]        Channel by number",
                    "/join <chan>      Join a channel",
                    "/leave <chan>     Leave a channel",
                    "/afk [msg]        Set AFK status",
                    "/dnd [msg]        Set Do Not Disturb",
                };
                for (const char* line : kChatHelp) {
                    game::MessageChatData helpMsg;
                    helpMsg.type = game::ChatType::SYSTEM;
                    helpMsg.language = game::ChatLanguage::UNIVERSAL;
                    helpMsg.message = line;
                    gameHandler.addLocalChatMessage(helpMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /macrohelp command — list available macro conditionals
            if (cmdLower == "macrohelp") {
                static constexpr const char* kMacroHelp[] = {
                    "--- Macro Conditionals ---",
                    "Usage: /cast [cond1,cond2] Spell1; [cond3] Spell2; Default",
                    "State:   [combat] [mounted] [swimming] [flying] [stealthed]",
                    "         [channeling] [pet] [group] [raid] [indoors] [outdoors]",
                    "Spec:    [spec:1] [spec:2]  (active talent spec, 1-based)",
                    "         (prefix no- to negate any condition)",
                    "Target:  [harm] [help] [exists] [noexists] [dead] [nodead]",
                    "         [target=focus] [target=pet] [target=mouseover] [target=player]",
                    "         (also: @focus, @pet, @mouseover, @player, @target)",
                    "Form:    [noform] [nostance] [form:0]",
                    "Keys:    [mod:shift] [mod:ctrl] [mod:alt]",
                    "Aura:    [buff:Name] [nobuff:Name] [debuff:Name] [nodebuff:Name]",
                    "Other:   #showtooltip, /stopmacro [cond], /castsequence",
                };
                for (const char* line : kMacroHelp) {
                    game::MessageChatData m;
                    m.type = game::ChatType::SYSTEM;
                    m.language = game::ChatLanguage::UNIVERSAL;
                    m.message = line;
                    gameHandler.addLocalChatMessage(m);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /help command — list available slash commands
            if (cmdLower == "help" || cmdLower == "?") {
                static constexpr const char* kHelpLines[] = {
                    "--- Wowee Slash Commands ---",
                    "Chat: /s /y /p /g /raid /rw /o /bg /w <name> /r  /join /leave",
                    "Social: /who  /friend add/remove  /ignore  /unignore",
                    "Party: /invite  /uninvite  /leave  /readycheck  /mark  /roll",
                    "       /maintank  /mainassist  /raidconvert  /raidinfo",
                    "       /lootmethod  /lootthreshold",
                    "Guild: /ginvite  /gkick  /gquit  /gpromote  /gdemote  /gmotd",
                    "       /gleader  /groster  /ginfo  /gcreate  /gdisband",
                    "Combat: /cast  /castsequence  /use  /startattack  /stopattack",
                    "        /stopcasting  /duel  /forfeit  /pvp  /assist",
                    "        /follow  /stopfollow  /threat  /combatlog",
                    "Items: /use <item>  /equip <item>  /equipset [name]",
                    "Target: /target  /cleartarget  /focus  /clearfocus  /inspect",
                    "Movement: /sit  /stand  /kneel  /dismount",
                    "Misc: /played  /time  /zone  /loc  /afk  /dnd  /helm  /cloak",
                    "      /trade  /score  /unstuck  /logout  /quit  /exit  /ticket",
                    "      /screenshot  /difficulty",
                    "      /macrohelp  /chathelp  /help",
                };
                for (const char* line : kHelpLines) {
                    game::MessageChatData helpMsg;
                    helpMsg.type = game::ChatType::SYSTEM;
                    helpMsg.language = game::ChatLanguage::UNIVERSAL;
                    helpMsg.message = line;
                    gameHandler.addLocalChatMessage(helpMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /who commands
            if (cmdLower == "who" || cmdLower == "whois" || cmdLower == "online" || cmdLower == "players") {
                std::string query;
                if (spacePos != std::string::npos) {
                    query = command.substr(spacePos + 1);
                    // Trim leading/trailing whitespace
                    size_t first = query.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos) {
                        query.clear();
                    } else {
                        size_t last = query.find_last_not_of(" \t\r\n");
                        query = query.substr(first, last - first + 1);
                    }
                }

                if ((cmdLower == "whois") && query.empty()) {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Usage: /whois <playerName>";
                    gameHandler.addLocalChatMessage(msg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                if (cmdLower == "who" && (query == "help" || query == "?")) {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Who commands: /who [name/filter], /whois <name>, /online";
                    gameHandler.addLocalChatMessage(msg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                gameHandler.queryWho(query);
                slashCmds_.showWho = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /combatlog command
            if (cmdLower == "combatlog" || cmdLower == "cl") {
                slashCmds_.toggleCombatLog = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /roll command
            if (cmdLower == "roll" || cmdLower == "random" || cmdLower == "rnd") {
                uint32_t minRoll = 1;
                uint32_t maxRoll = 100;

                if (spacePos != std::string::npos) {
                    std::string args = command.substr(spacePos + 1);
                    size_t dashPos = args.find('-');
                    size_t spacePos2 = args.find(' ');

                    if (dashPos != std::string::npos) {
                        // Format: /roll 1-100
                        try {
                            minRoll = std::stoul(args.substr(0, dashPos));
                            maxRoll = std::stoul(args.substr(dashPos + 1));
                        } catch (...) {}
                    } else if (spacePos2 != std::string::npos) {
                        // Format: /roll 1 100
                        try {
                            minRoll = std::stoul(args.substr(0, spacePos2));
                            maxRoll = std::stoul(args.substr(spacePos2 + 1));
                        } catch (...) {}
                    } else {
                        // Format: /roll 100 (means 1-100)
                        try {
                            maxRoll = std::stoul(args);
                        } catch (...) {}
                    }
                }

                gameHandler.randomRoll(minRoll, maxRoll);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /friend or /addfriend command
            if (cmdLower == "friend" || cmdLower == "addfriend") {
                if (spacePos != std::string::npos) {
                    std::string args = command.substr(spacePos + 1);
                    size_t subCmdSpace = args.find(' ');

                    if (cmdLower == "friend" && subCmdSpace != std::string::npos) {
                        std::string subCmd = args.substr(0, subCmdSpace);
                        std::transform(subCmd.begin(), subCmd.end(), subCmd.begin(), ::tolower);

                        if (subCmd == "add") {
                            std::string playerName = args.substr(subCmdSpace + 1);
                            gameHandler.addFriend(playerName);
                            chatInputBuffer_[0] = '\0';
                            return;
                        } else if (subCmd == "remove" || subCmd == "delete" || subCmd == "rem") {
                            std::string playerName = args.substr(subCmdSpace + 1);
                            gameHandler.removeFriend(playerName);
                            chatInputBuffer_[0] = '\0';
                            return;
                        }
                    } else {
                        // /addfriend name or /friend name (assume add)
                        gameHandler.addFriend(args);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /friend add <name> or /friend remove <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /removefriend or /delfriend command
            if (cmdLower == "removefriend" || cmdLower == "delfriend" || cmdLower == "remfriend") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.removeFriend(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /removefriend <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ignore command
            if (cmdLower == "ignore") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.addIgnore(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /ignore <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /unignore command
            if (cmdLower == "unignore") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.removeIgnore(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /unignore <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /dismount command
            if (cmdLower == "dismount") {
                gameHandler.dismount();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Pet control commands (common macro use)
            // Action IDs: 1=passive, 2=follow, 3=stay, 4=defensive, 5=attack, 6=aggressive
            if (cmdLower == "petattack") {
                uint64_t target = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                gameHandler.sendPetAction(5, target);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petfollow") {
                gameHandler.sendPetAction(2, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petstay" || cmdLower == "pethalt") {
                gameHandler.sendPetAction(3, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petpassive") {
                gameHandler.sendPetAction(1, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petdefensive") {
                gameHandler.sendPetAction(4, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petaggressive") {
                gameHandler.sendPetAction(6, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petdismiss") {
                gameHandler.dismissPet();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cancelform / /cancelshapeshift — leave current shapeshift/stance
            if (cmdLower == "cancelform" || cmdLower == "cancelshapeshift") {
                // Cancel the first permanent shapeshift aura the player has
                for (const auto& aura : gameHandler.getPlayerAuras()) {
                    if (aura.spellId == 0) continue;
                    // Permanent shapeshift auras have the permanent flag (0x20) set
                    if (aura.flags & 0x20) {
                        gameHandler.cancelAura(aura.spellId);
                        break;
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cancelaura <spell name|#id> — cancel a specific buff by name or ID
            if (cmdLower == "cancelaura" && spacePos != std::string::npos) {
                std::string auraArg = command.substr(spacePos + 1);
                while (!auraArg.empty() && auraArg.front() == ' ') auraArg.erase(auraArg.begin());
                while (!auraArg.empty() && auraArg.back()  == ' ') auraArg.pop_back();
                // Try numeric ID first
                {
                    std::string numStr = auraArg;
                    if (!numStr.empty() && numStr.front() == '#') numStr.erase(numStr.begin());
                    bool isNum = !numStr.empty() &&
                        std::all_of(numStr.begin(), numStr.end(),
                                    [](unsigned char c){ return std::isdigit(c); });
                    if (isNum) {
                        uint32_t spellId = 0;
                        try { spellId = static_cast<uint32_t>(std::stoul(numStr)); } catch (...) {}
                        if (spellId) gameHandler.cancelAura(spellId);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }
                // Name match against player auras
                std::string argLow = auraArg;
                for (char& c : argLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (const auto& aura : gameHandler.getPlayerAuras()) {
                    if (aura.spellId == 0) continue;
                    std::string sn = gameHandler.getSpellName(aura.spellId);
                    for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (sn == argLow) {
                        gameHandler.cancelAura(aura.spellId);
                        break;
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /sit command
            if (cmdLower == "sit") {
                gameHandler.setStandState(1);  // 1 = sit
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /stand command
            if (cmdLower == "stand") {
                gameHandler.setStandState(0);  // 0 = stand
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /kneel command
            if (cmdLower == "kneel") {
                gameHandler.setStandState(8);  // 8 = kneel
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /logout command (also /camp, /quit, /exit)
            if (cmdLower == "logout" || cmdLower == "camp" || cmdLower == "quit" || cmdLower == "exit") {
                gameHandler.requestLogout();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cancellogout command
            if (cmdLower == "cancellogout") {
                gameHandler.cancelLogout();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /difficulty command — set dungeon/raid difficulty (WotLK)
            if (cmdLower == "difficulty") {
                std::string arg;
                if (spacePos != std::string::npos) {
                    arg = command.substr(spacePos + 1);
                    // Trim whitespace
                    size_t first = arg.find_first_not_of(" \t");
                    size_t last  = arg.find_last_not_of(" \t");
                    if (first != std::string::npos)
                        arg = arg.substr(first, last - first + 1);
                    else
                        arg.clear();
                    for (auto& ch : arg) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }

                uint32_t diff = 0;
                bool valid = true;
                if (arg == "normal" || arg == "0")         diff = 0;
                else if (arg == "heroic" || arg == "1")    diff = 1;
                else if (arg == "25" || arg == "25normal" || arg == "25man" || arg == "2")
                    diff = 2;
                else if (arg == "25heroic" || arg == "25manheroic" || arg == "3")
                    diff = 3;
                else valid = false;

                if (!valid || arg.empty()) {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Usage: /difficulty normal|heroic|25|25heroic  (0-3)";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    static constexpr const char* kDiffNames[] = {
                        "Normal (5-man)", "Heroic (5-man)", "Normal (25-man)", "Heroic (25-man)"
                    };
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = std::string("Setting difficulty to: ") + kDiffNames[diff];
                    gameHandler.addLocalChatMessage(msg);
                    gameHandler.sendSetDifficulty(diff);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /helm command
            if (cmdLower == "helm" || cmdLower == "helmet" || cmdLower == "showhelm") {
                gameHandler.toggleHelm();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cloak command
            if (cmdLower == "cloak" || cmdLower == "showcloak") {
                gameHandler.toggleCloak();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /follow command
            if (cmdLower == "follow" || cmdLower == "f") {
                gameHandler.followTarget();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /stopfollow command
            if (cmdLower == "stopfollow") {
                gameHandler.cancelFollow();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /assist command
            if (cmdLower == "assist") {
                // /assist              → assist current target (use their target)
                // /assist PlayerName   → find PlayerName, target their target
                // /assist [target=X]   → evaluate conditional, target that entity's target
                auto assistEntityTarget = [&](uint64_t srcGuid) {
                    auto srcEnt = gameHandler.getEntityManager().getEntity(srcGuid);
                    if (!srcEnt) { gameHandler.assistTarget(); return; }
                    uint64_t atkGuid = 0;
                    const auto& flds = srcEnt->getFields();
                    auto iLo = flds.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
                    if (iLo != flds.end()) {
                        atkGuid = iLo->second;
                        auto iHi = flds.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                        if (iHi != flds.end()) atkGuid |= (static_cast<uint64_t>(iHi->second) << 32);
                    }
                    if (atkGuid != 0) {
                        gameHandler.setTarget(atkGuid);
                    } else {
                        std::string sn = getEntityName(srcEnt);
                        game::MessageChatData msg;
                        msg.type = game::ChatType::SYSTEM;
                        msg.language = game::ChatLanguage::UNIVERSAL;
                        msg.message = (sn.empty() ? "Target" : sn) + " has no target.";
                        gameHandler.addLocalChatMessage(msg);
                    }
                };

                if (spacePos != std::string::npos) {
                    std::string assistArg = command.substr(spacePos + 1);
                    while (!assistArg.empty() && assistArg.front() == ' ') assistArg.erase(assistArg.begin());

                    // Evaluate conditionals if present
                    uint64_t assistOver = static_cast<uint64_t>(-1);
                    if (!assistArg.empty() && assistArg.front() == '[') {
                        assistArg = evaluateMacroConditionals(assistArg, gameHandler, assistOver);
                        if (assistArg.empty() && assistOver == static_cast<uint64_t>(-1)) {
                            chatInputBuffer_[0] = '\0'; return;  // no condition matched
                        }
                        while (!assistArg.empty() && assistArg.front() == ' ') assistArg.erase(assistArg.begin());
                        while (!assistArg.empty() && assistArg.back()  == ' ') assistArg.pop_back();
                    }

                    if (assistOver != static_cast<uint64_t>(-1) && assistOver != 0) {
                        assistEntityTarget(assistOver);
                    } else if (!assistArg.empty()) {
                        // Name search
                        std::string argLow = assistArg;
                        for (char& c : argLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        uint64_t bestGuid = 0; float bestDist = std::numeric_limits<float>::max();
                        const auto& pmi = gameHandler.getMovementInfo();
                        for (const auto& [guid, ent] : gameHandler.getEntityManager().getEntities()) {
                            if (!ent || ent->getType() == game::ObjectType::OBJECT) continue;
                            std::string nm = getEntityName(ent);
                            std::string nml = nm;
                            for (char& c : nml) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (nml.find(argLow) != 0) continue;
                            float d2 = (ent->getX()-pmi.x)*(ent->getX()-pmi.x)
                                     + (ent->getY()-pmi.y)*(ent->getY()-pmi.y);
                            if (d2 < bestDist) { bestDist = d2; bestGuid = guid; }
                        }
                        if (bestGuid) assistEntityTarget(bestGuid);
                        else {
                            game::MessageChatData msg;
                            msg.type = game::ChatType::SYSTEM;
                            msg.language = game::ChatLanguage::UNIVERSAL;
                            msg.message = "No unit matching '" + assistArg + "' found.";
                            gameHandler.addLocalChatMessage(msg);
                        }
                    } else {
                        gameHandler.assistTarget();
                    }
                } else {
                    gameHandler.assistTarget();
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /pvp command
            if (cmdLower == "pvp") {
                gameHandler.togglePvp();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ginfo command
            if (cmdLower == "ginfo" || cmdLower == "guildinfo") {
                gameHandler.requestGuildInfo();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /groster command
            if (cmdLower == "groster" || cmdLower == "guildroster") {
                gameHandler.requestGuildRoster();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gmotd command
            if (cmdLower == "gmotd" || cmdLower == "guildmotd") {
                if (spacePos != std::string::npos) {
                    std::string motd = command.substr(spacePos + 1);
                    gameHandler.setGuildMotd(motd);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gmotd <message>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gpromote command
            if (cmdLower == "gpromote" || cmdLower == "guildpromote") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.promoteGuildMember(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gpromote <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gdemote command
            if (cmdLower == "gdemote" || cmdLower == "guilddemote") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.demoteGuildMember(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gdemote <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gquit command
            if (cmdLower == "gquit" || cmdLower == "guildquit" || cmdLower == "leaveguild") {
                gameHandler.leaveGuild();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ginvite command
            if (cmdLower == "ginvite" || cmdLower == "guildinvite") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.inviteToGuild(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /ginvite <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gkick command
            if (cmdLower == "gkick" || cmdLower == "guildkick") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.kickGuildMember(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gkick <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gcreate command
            if (cmdLower == "gcreate" || cmdLower == "guildcreate") {
                if (spacePos != std::string::npos) {
                    std::string guildName = command.substr(spacePos + 1);
                    gameHandler.createGuild(guildName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gcreate <guild name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gdisband command
            if (cmdLower == "gdisband" || cmdLower == "guilddisband") {
                gameHandler.disbandGuild();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gleader command
            if (cmdLower == "gleader" || cmdLower == "guildleader") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.setGuildLeader(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gleader <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /readycheck command
            if (cmdLower == "readycheck" || cmdLower == "rc") {
                gameHandler.initiateReadyCheck();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ready command (respond yes to ready check)
            if (cmdLower == "ready") {
                gameHandler.respondToReadyCheck(true);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /notready command (respond no to ready check)
            if (cmdLower == "notready" || cmdLower == "nr") {
                gameHandler.respondToReadyCheck(false);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /yield or /forfeit command
            if (cmdLower == "yield" || cmdLower == "forfeit" || cmdLower == "surrender") {
                gameHandler.forfeitDuel();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // AFK command
            if (cmdLower == "afk" || cmdLower == "away") {
                std::string afkMsg = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                gameHandler.toggleAfk(afkMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // DND command
            if (cmdLower == "dnd" || cmdLower == "busy") {
                std::string dndMsg = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                gameHandler.toggleDnd(dndMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Reply command
            if (cmdLower == "r" || cmdLower == "reply") {
                std::string lastSender = gameHandler.getLastWhisperSender();
                if (lastSender.empty()) {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.language = game::ChatLanguage::UNIVERSAL;
                    errMsg.message = "No one has whispered you yet.";
                    gameHandler.addLocalChatMessage(errMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                // Set whisper target to last whisper sender
                strncpy(whisperTargetBuffer_, lastSender.c_str(), sizeof(whisperTargetBuffer_) - 1);
                whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                if (spacePos != std::string::npos) {
                    // /r message — send reply immediately
                    std::string replyMsg = command.substr(spacePos + 1);
                    gameHandler.sendChatMessage(game::ChatType::WHISPER, replyMsg, lastSender);
                }
                // Switch to whisper tab
                selectedChatType_ = 4;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Party/Raid management commands
            if (cmdLower == "uninvite" || cmdLower == "kick") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.uninvitePlayer(playerName);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Usage: /uninvite <player name>";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "leave" || cmdLower == "leaveparty") {
                gameHandler.leaveParty();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "maintank" || cmdLower == "mt") {
                if (gameHandler.hasTarget()) {
                    gameHandler.setMainTank(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to set as main tank.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "mainassist" || cmdLower == "ma") {
                if (gameHandler.hasTarget()) {
                    gameHandler.setMainAssist(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to set as main assist.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clearmaintank") {
                gameHandler.clearMainTank();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clearmainassist") {
                gameHandler.clearMainAssist();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "raidinfo") {
                gameHandler.requestRaidInfo();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "raidconvert") {
                gameHandler.convertToRaid();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /lootmethod (or /grouploot, /setloot) — set party/raid loot method
            if (cmdLower == "lootmethod" || cmdLower == "grouploot" || cmdLower == "setloot") {
                if (!gameHandler.isInGroup()) {
                    gameHandler.addUIError("You are not in a group.");
                } else if (spacePos == std::string::npos) {
                    // No argument — show current method and usage
                    static constexpr const char* kMethodNames[] = {
                        "Free for All", "Round Robin", "Master Looter", "Group Loot", "Need Before Greed"
                    };
                    const auto& pd = gameHandler.getPartyData();
                    const char* cur = (pd.lootMethod < 5) ? kMethodNames[pd.lootMethod] : "Unknown";
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = std::string("Current loot method: ") + cur;
                    gameHandler.addLocalChatMessage(msg);
                    msg.message = "Usage: /lootmethod ffa|roundrobin|master|group|needbeforegreed";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    std::string arg = command.substr(spacePos + 1);
                    // Lowercase the argument
                    for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    uint32_t method = 0xFFFFFFFF;
                    if (arg == "ffa" || arg == "freeforall")         method = 0;
                    else if (arg == "roundrobin" || arg == "rr")     method = 1;
                    else if (arg == "master" || arg == "masterloot") method = 2;
                    else if (arg == "group" || arg == "grouploot")   method = 3;
                    else if (arg == "needbeforegreed" || arg == "nbg" || arg == "need") method = 4;

                    if (method == 0xFFFFFFFF) {
                        gameHandler.addUIError("Unknown loot method. Use: ffa, roundrobin, master, group, needbeforegreed");
                    } else {
                        const auto& pd = gameHandler.getPartyData();
                        // Master loot uses player guid as master looter; otherwise 0
                        uint64_t masterGuid = (method == 2) ? gameHandler.getPlayerGuid() : 0;
                        gameHandler.sendSetLootMethod(method, pd.lootThreshold, masterGuid);
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /lootthreshold — set minimum item quality for group loot rolls
            if (cmdLower == "lootthreshold") {
                if (!gameHandler.isInGroup()) {
                    gameHandler.addUIError("You are not in a group.");
                } else if (spacePos == std::string::npos) {
                    const auto& pd = gameHandler.getPartyData();
                    static constexpr const char* kQualityNames[] = {
                        "Poor (grey)", "Common (white)", "Uncommon (green)",
                        "Rare (blue)", "Epic (purple)", "Legendary (orange)"
                    };
                    const char* cur = (pd.lootThreshold < 6) ? kQualityNames[pd.lootThreshold] : "Unknown";
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = std::string("Current loot threshold: ") + cur;
                    gameHandler.addLocalChatMessage(msg);
                    msg.message = "Usage: /lootthreshold <0-5> (0=Poor, 1=Common, 2=Uncommon, 3=Rare, 4=Epic, 5=Legendary)";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    std::string arg = command.substr(spacePos + 1);
                    // Trim whitespace
                    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
                    uint32_t threshold = 0xFFFFFFFF;
                    if (arg.size() == 1 && arg[0] >= '0' && arg[0] <= '5') {
                        threshold = static_cast<uint32_t>(arg[0] - '0');
                    } else {
                        // Accept quality names
                        for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (arg == "poor" || arg == "grey" || arg == "gray") threshold = 0;
                        else if (arg == "common" || arg == "white")          threshold = 1;
                        else if (arg == "uncommon" || arg == "green")        threshold = 2;
                        else if (arg == "rare" || arg == "blue")             threshold = 3;
                        else if (arg == "epic" || arg == "purple")           threshold = 4;
                        else if (arg == "legendary" || arg == "orange")      threshold = 5;
                    }

                    if (threshold == 0xFFFFFFFF) {
                        gameHandler.addUIError("Invalid threshold. Use 0-5 or: poor, common, uncommon, rare, epic, legendary");
                    } else {
                        const auto& pd = gameHandler.getPartyData();
                        uint64_t masterGuid = (pd.lootMethod == 2) ? gameHandler.getPlayerGuid() : 0;
                        gameHandler.sendSetLootMethod(pd.lootMethod, threshold, masterGuid);
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /mark [icon] — set or clear a raid target mark on the current target.
            // Icon names (case-insensitive): star, circle, diamond, triangle, moon, square, cross, skull
            // /mark clear | /mark 0 — remove all marks (sets icon 0xFF = clear)
            // /mark — no arg marks with skull (icon 7)
            if (cmdLower == "mark" || cmdLower == "marktarget" || cmdLower == "raidtarget") {
                if (!gameHandler.hasTarget()) {
                    game::MessageChatData noTgt;
                    noTgt.type = game::ChatType::SYSTEM;
                    noTgt.language = game::ChatLanguage::UNIVERSAL;
                    noTgt.message = "No target selected.";
                    gameHandler.addLocalChatMessage(noTgt);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                static constexpr const char* kMarkWords[] = {
                    "star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull"
                };
                uint8_t icon = 7; // default: skull
                if (spacePos != std::string::npos) {
                    std::string arg = command.substr(spacePos + 1);
                    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
                    std::string argLow = arg;
                    for (auto& c : argLow) c = static_cast<char>(std::tolower(c));
                    if (argLow == "clear" || argLow == "0" || argLow == "none") {
                        gameHandler.setRaidMark(gameHandler.getTargetGuid(), 0xFF);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                    bool found = false;
                    for (int mi = 0; mi < 8; ++mi) {
                        if (argLow == kMarkWords[mi]) { icon = static_cast<uint8_t>(mi); found = true; break; }
                    }
                    if (!found && !argLow.empty() && argLow[0] >= '1' && argLow[0] <= '8') {
                        icon = static_cast<uint8_t>(argLow[0] - '1');
                        found = true;
                    }
                    if (!found) {
                        game::MessageChatData badArg;
                        badArg.type = game::ChatType::SYSTEM;
                        badArg.language = game::ChatLanguage::UNIVERSAL;
                        badArg.message = "Unknown mark. Use: star circle diamond triangle moon square cross skull";
                        gameHandler.addLocalChatMessage(badArg);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }
                gameHandler.setRaidMark(gameHandler.getTargetGuid(), icon);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Combat and Trade commands
            if (cmdLower == "duel") {
                if (gameHandler.hasTarget()) {
                    gameHandler.proposeDuel(gameHandler.getTargetGuid());
                } else if (spacePos != std::string::npos) {
                    // Target player by name (would need name-to-GUID lookup)
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to challenge to a duel.";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to challenge to a duel.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "trade") {
                if (gameHandler.hasTarget()) {
                    gameHandler.initiateTrade(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to trade with.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "startattack") {
                // Support macro conditionals: /startattack [harm,nodead]
                bool condPass = true;
                uint64_t saOverride = static_cast<uint64_t>(-1);
                if (spacePos != std::string::npos) {
                    std::string saArg = command.substr(spacePos + 1);
                    while (!saArg.empty() && saArg.front() == ' ') saArg.erase(saArg.begin());
                    if (!saArg.empty() && saArg.front() == '[') {
                        std::string result = evaluateMacroConditionals(saArg, gameHandler, saOverride);
                        condPass = !(result.empty() && saOverride == static_cast<uint64_t>(-1));
                    }
                }
                if (condPass) {
                    uint64_t atkTarget = (saOverride != static_cast<uint64_t>(-1) && saOverride != 0)
                        ? saOverride : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);
                    if (atkTarget != 0) {
                        gameHandler.startAutoAttack(atkTarget);
                    } else {
                        game::MessageChatData msg;
                        msg.type = game::ChatType::SYSTEM;
                        msg.language = game::ChatLanguage::UNIVERSAL;
                        msg.message = "You have no target.";
                        gameHandler.addLocalChatMessage(msg);
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "stopattack") {
                gameHandler.stopAutoAttack();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "stopcasting") {
                gameHandler.stopCasting();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "cancelqueuedspell" || cmdLower == "stopspellqueue") {
                gameHandler.cancelQueuedSpell();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /equipset [name] — equip a saved equipment set by name (partial match, case-insensitive)
            // /equipset          — list available sets in chat
            if (cmdLower == "equipset") {
                const auto& sets = gameHandler.getEquipmentSets();
                auto sysSay = [&](const std::string& msg) {
                    game::MessageChatData m;
                    m.type = game::ChatType::SYSTEM;
                    m.language = game::ChatLanguage::UNIVERSAL;
                    m.message = msg;
                    gameHandler.addLocalChatMessage(m);
                };
                if (spacePos == std::string::npos) {
                    // No argument: list available sets
                    if (sets.empty()) {
                        sysSay("[System] No equipment sets saved.");
                    } else {
                        sysSay("[System] Equipment sets:");
                        for (const auto& es : sets)
                            sysSay("  " + es.name);
                    }
                } else {
                    std::string setName = command.substr(spacePos + 1);
                    while (!setName.empty() && setName.front() == ' ') setName.erase(setName.begin());
                    while (!setName.empty() && setName.back()  == ' ') setName.pop_back();
                    // Case-insensitive prefix match
                    std::string setLower = setName;
                    std::transform(setLower.begin(), setLower.end(), setLower.begin(), ::tolower);
                    const game::GameHandler::EquipmentSetInfo* found = nullptr;
                    for (const auto& es : sets) {
                        std::string nameLow = es.name;
                        std::transform(nameLow.begin(), nameLow.end(), nameLow.begin(), ::tolower);
                        if (nameLow == setLower || nameLow.find(setLower) == 0) {
                            found = &es;
                            break;
                        }
                    }
                    if (found) {
                        gameHandler.useEquipmentSet(found->setId);
                    } else {
                        sysSay("[System] No equipment set matching '" + setName + "'.");
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /castsequence [conds] [reset=N/target/combat] Spell1, Spell2, ...
            // Cycles through the spell list on successive presses; resets per the reset= spec.
            if (cmdLower == "castsequence" && spacePos != std::string::npos) {
                std::string seqArg = command.substr(spacePos + 1);
                while (!seqArg.empty() && seqArg.front() == ' ') seqArg.erase(seqArg.begin());

                // Macro conditionals
                uint64_t seqTgtOver = static_cast<uint64_t>(-1);
                if (!seqArg.empty() && seqArg.front() == '[') {
                    seqArg = evaluateMacroConditionals(seqArg, gameHandler, seqTgtOver);
                    if (seqArg.empty() && seqTgtOver == static_cast<uint64_t>(-1)) {
                        chatInputBuffer_[0] = '\0'; return;
                    }
                    while (!seqArg.empty() && seqArg.front() == ' ') seqArg.erase(seqArg.begin());
                    while (!seqArg.empty() && seqArg.back()  == ' ') seqArg.pop_back();
                }

                // Optional reset= spec (may contain slash-separated conditions: reset=5/target)
                std::string resetSpec;
                if (seqArg.rfind("reset=", 0) == 0) {
                    size_t spAfter = seqArg.find(' ');
                    if (spAfter != std::string::npos) {
                        resetSpec = seqArg.substr(6, spAfter - 6);
                        seqArg = seqArg.substr(spAfter + 1);
                        while (!seqArg.empty() && seqArg.front() == ' ') seqArg.erase(seqArg.begin());
                    }
                }

                // Parse comma-separated spell list
                std::vector<std::string> seqSpells;
                {
                    std::string cur;
                    for (char c : seqArg) {
                        if (c == ',') {
                            while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
                            while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                            if (!cur.empty()) seqSpells.push_back(cur);
                            cur.clear();
                        } else { cur += c; }
                    }
                    while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
                    while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                    if (!cur.empty()) seqSpells.push_back(cur);
                }
                if (seqSpells.empty()) { chatInputBuffer_[0] = '\0'; return; }

                // Build stable key from lowercase spell list
                std::string seqKey;
                for (size_t k = 0; k < seqSpells.size(); ++k) {
                    if (k) seqKey += ',';
                    std::string sl = seqSpells[k];
                    for (char& c : sl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    seqKey += sl;
                }

                auto& seqState = s_castSeqStates[seqKey];

                // Check reset conditions (slash-separated: e.g. "5/target")
                float nowSec = static_cast<float>(ImGui::GetTime());
                bool shouldReset = false;
                if (!resetSpec.empty()) {
                    size_t rpos = 0;
                    while (rpos <= resetSpec.size()) {
                        size_t slash = resetSpec.find('/', rpos);
                        std::string part = (slash != std::string::npos)
                            ? resetSpec.substr(rpos, slash - rpos)
                            : resetSpec.substr(rpos);
                        std::string plow = part;
                        for (char& c : plow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        bool isNum = !plow.empty() && std::all_of(plow.begin(), plow.end(),
                            [](unsigned char c){ return std::isdigit(c) || c == '.'; });
                        if (isNum) {
                            float rSec = 0.0f;
                            try { rSec = std::stof(plow); } catch (...) {}
                            if (rSec > 0.0f && nowSec - seqState.lastPressSec > rSec) shouldReset = true;
                        } else if (plow == "target") {
                            if (gameHandler.getTargetGuid() != seqState.lastTargetGuid) shouldReset = true;
                        } else if (plow == "combat") {
                            if (gameHandler.isInCombat() != seqState.lastInCombat) shouldReset = true;
                        }
                        if (slash == std::string::npos) break;
                        rpos = slash + 1;
                    }
                }
                if (shouldReset || seqState.index >= seqSpells.size()) seqState.index = 0;

                const std::string& seqSpell = seqSpells[seqState.index];
                seqState.index = (seqState.index + 1) % seqSpells.size();
                seqState.lastPressSec  = nowSec;
                seqState.lastTargetGuid = gameHandler.getTargetGuid();
                seqState.lastInCombat   = gameHandler.isInCombat();

                // Cast the selected spell — mirrors /cast spell lookup
                std::string ssLow = seqSpell;
                for (char& c : ssLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!ssLow.empty() && ssLow.front() == '!') ssLow.erase(ssLow.begin());

                uint64_t seqTargetGuid = (seqTgtOver != static_cast<uint64_t>(-1) && seqTgtOver != 0)
                    ? seqTgtOver : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);

                // Numeric ID
                if (!ssLow.empty() && ssLow.front() == '#') ssLow.erase(ssLow.begin());
                bool ssNumeric = !ssLow.empty() && std::all_of(ssLow.begin(), ssLow.end(),
                    [](unsigned char c){ return std::isdigit(c); });
                if (ssNumeric) {
                    uint32_t ssId = 0;
                    try { ssId = static_cast<uint32_t>(std::stoul(ssLow)); } catch (...) {}
                    if (ssId) gameHandler.castSpell(ssId, seqTargetGuid);
                } else {
                    uint32_t ssBest = 0; int ssBestRank = -1;
                    for (uint32_t sid : gameHandler.getKnownSpells()) {
                        const std::string& sn = gameHandler.getSpellName(sid);
                        if (sn.empty()) continue;
                        std::string snl = sn;
                        for (char& c : snl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (snl != ssLow) continue;
                        int sRnk = 0;
                        const std::string& rk = gameHandler.getSpellRank(sid);
                        if (!rk.empty()) {
                            std::string rkl = rk;
                            for (char& c : rkl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (rkl.rfind("rank ", 0) == 0) { try { sRnk = std::stoi(rkl.substr(5)); } catch (...) {} }
                        }
                        if (sRnk > ssBestRank) { ssBestRank = sRnk; ssBest = sid; }
                    }
                    if (ssBest) gameHandler.castSpell(ssBest, seqTargetGuid);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "cast" && spacePos != std::string::npos) {
                std::string spellArg = command.substr(spacePos + 1);
                // Trim leading/trailing whitespace
                while (!spellArg.empty() && spellArg.front() == ' ') spellArg.erase(spellArg.begin());
                while (!spellArg.empty() && spellArg.back()  == ' ') spellArg.pop_back();

                // Evaluate WoW macro conditionals: /cast [mod:shift] Greater Heal; Flash Heal
                uint64_t castTargetOverride = static_cast<uint64_t>(-1);
                if (!spellArg.empty() && spellArg.front() == '[') {
                    spellArg = evaluateMacroConditionals(spellArg, gameHandler, castTargetOverride);
                    if (spellArg.empty()) {
                        chatInputBuffer_[0] = '\0';
                        return;  // No conditional matched — skip cast
                    }
                    while (!spellArg.empty() && spellArg.front() == ' ') spellArg.erase(spellArg.begin());
                    while (!spellArg.empty() && spellArg.back()  == ' ') spellArg.pop_back();
                }

                // Strip leading '!' (WoW /cast !Spell forces recast without toggling off)
                if (!spellArg.empty() && spellArg.front() == '!') spellArg.erase(spellArg.begin());

                // Support numeric spell ID: /cast 133 or /cast #133
                {
                    std::string numStr = spellArg;
                    if (!numStr.empty() && numStr.front() == '#') numStr.erase(numStr.begin());
                    bool isNumeric = !numStr.empty() &&
                        std::all_of(numStr.begin(), numStr.end(),
                                    [](unsigned char c){ return std::isdigit(c); });
                    if (isNumeric) {
                        uint32_t spellId = 0;
                        try { spellId = static_cast<uint32_t>(std::stoul(numStr)); } catch (...) {}
                        if (spellId != 0) {
                            uint64_t targetGuid = (castTargetOverride != static_cast<uint64_t>(-1))
                                ? castTargetOverride
                                : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);
                            gameHandler.castSpell(spellId, targetGuid);
                        }
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }

                // Parse optional "(Rank N)" suffix: "Fireball(Rank 3)" or "Fireball (Rank 3)"
                int requestedRank = -1;  // -1 = highest rank
                std::string spellName = spellArg;
                {
                    auto rankPos = spellArg.find('(');
                    if (rankPos != std::string::npos) {
                        std::string rankStr = spellArg.substr(rankPos + 1);
                        // Strip closing paren and whitespace
                        auto closePos = rankStr.find(')');
                        if (closePos != std::string::npos) rankStr = rankStr.substr(0, closePos);
                        for (char& c : rankStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        // Expect "rank N"
                        if (rankStr.rfind("rank ", 0) == 0) {
                            try { requestedRank = std::stoi(rankStr.substr(5)); } catch (...) {}
                        }
                        spellName = spellArg.substr(0, rankPos);
                        while (!spellName.empty() && spellName.back() == ' ') spellName.pop_back();
                    }
                }

                std::string spellNameLower = spellName;
                for (char& c : spellNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                // Search known spells for a name match; pick highest rank (or specific rank)
                uint32_t bestSpellId = 0;
                int bestRank = -1;
                for (uint32_t sid : gameHandler.getKnownSpells()) {
                    const std::string& sName = gameHandler.getSpellName(sid);
                    if (sName.empty()) continue;
                    std::string sNameLower = sName;
                    for (char& c : sNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (sNameLower != spellNameLower) continue;

                    // Parse numeric rank from rank string ("Rank 3" → 3, "" → 0)
                    int sRank = 0;
                    const std::string& rankStr = gameHandler.getSpellRank(sid);
                    if (!rankStr.empty()) {
                        std::string rLow = rankStr;
                        for (char& c : rLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (rLow.rfind("rank ", 0) == 0) {
                            try { sRank = std::stoi(rLow.substr(5)); } catch (...) {}
                        }
                    }

                    if (requestedRank >= 0) {
                        if (sRank == requestedRank) { bestSpellId = sid; break; }
                    } else {
                        if (sRank > bestRank) { bestRank = sRank; bestSpellId = sid; }
                    }
                }

                if (bestSpellId) {
                    uint64_t targetGuid = (castTargetOverride != static_cast<uint64_t>(-1))
                        ? castTargetOverride
                        : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);
                    gameHandler.castSpell(bestSpellId, targetGuid);
                } else {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = requestedRank >= 0
                        ? "You don't know '" + spellName + "' (Rank " + std::to_string(requestedRank) + ")."
                        : "Unknown spell: '" + spellName + "'.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /use <item name|#id|bag slot|equip slot>
            // Supports: item name, numeric item ID (#N or N), bag/slot (/use 0 1 = backpack slot 1,
            //           /use 1-4 slot = bag slot), equipment slot number (/use 16 = main hand)
            if (cmdLower == "use" && spacePos != std::string::npos) {
                std::string useArg = command.substr(spacePos + 1);
                while (!useArg.empty() && useArg.front() == ' ') useArg.erase(useArg.begin());
                while (!useArg.empty() && useArg.back()  == ' ') useArg.pop_back();

                // Handle macro conditionals: /use [mod:shift] ItemName; OtherItem
                if (!useArg.empty() && useArg.front() == '[') {
                    uint64_t dummy = static_cast<uint64_t>(-1);
                    useArg = evaluateMacroConditionals(useArg, gameHandler, dummy);
                    if (useArg.empty()) { chatInputBuffer_[0] = '\0'; return; }
                    while (!useArg.empty() && useArg.front() == ' ') useArg.erase(useArg.begin());
                    while (!useArg.empty() && useArg.back()  == ' ') useArg.pop_back();
                }

                // Check for bag/slot notation: two numbers separated by whitespace
                {
                    std::istringstream iss(useArg);
                    int bagNum = -1, slotNum = -1;
                    iss >> bagNum >> slotNum;
                    if (!iss.fail() && slotNum >= 1) {
                        if (bagNum == 0) {
                            // Backpack: bag=0, slot 1-based → 0-based
                            gameHandler.useItemBySlot(slotNum - 1);
                            chatInputBuffer_[0] = '\0';
                            return;
                        } else if (bagNum >= 1 && bagNum <= game::Inventory::NUM_BAG_SLOTS) {
                            // Equip bag: bags are 1-indexed (bag 1 = bagIndex 0)
                            gameHandler.useItemInBag(bagNum - 1, slotNum - 1);
                            chatInputBuffer_[0] = '\0';
                            return;
                        }
                    }
                }

                // Numeric equip slot: /use 16 = slot 16 (1-based, WoW equip slot enum)
                {
                    std::string numStr = useArg;
                    if (!numStr.empty() && numStr.front() == '#') numStr.erase(numStr.begin());
                    bool isNumeric = !numStr.empty() &&
                        std::all_of(numStr.begin(), numStr.end(),
                                    [](unsigned char c){ return std::isdigit(c); });
                    if (isNumeric) {
                        // Treat as equip slot (1-based, maps to EquipSlot enum 0-based)
                        int slotNum = 0;
                        try { slotNum = std::stoi(numStr); } catch (...) {}
                        if (slotNum >= 1 && slotNum <= static_cast<int>(game::EquipSlot::BAG4) + 1) {
                            auto eslot = static_cast<game::EquipSlot>(slotNum - 1);
                            const auto& esl = gameHandler.getInventory().getEquipSlot(eslot);
                            if (!esl.empty())
                                gameHandler.useItemById(esl.item.itemId);
                        }
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }

                std::string useArgLower = useArg;
                for (char& c : useArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                bool found = false;
                const auto& inv = gameHandler.getInventory();
                // Search backpack
                for (int s = 0; s < inv.getBackpackSize() && !found; ++s) {
                    const auto& slot = inv.getBackpackSlot(s);
                    if (slot.empty()) continue;
                    const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                    if (!info) continue;
                    std::string nameLow = info->name;
                    for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (nameLow == useArgLower) {
                        gameHandler.useItemBySlot(s);
                        found = true;
                    }
                }
                // Search bags
                for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS && !found; ++b) {
                    for (int s = 0; s < inv.getBagSize(b) && !found; ++s) {
                        const auto& slot = inv.getBagSlot(b, s);
                        if (slot.empty()) continue;
                        const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                        if (!info) continue;
                        std::string nameLow = info->name;
                        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (nameLow == useArgLower) {
                            gameHandler.useItemInBag(b, s);
                            found = true;
                        }
                    }
                }
                if (!found) {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "Item not found: '" + useArg + "'.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /equip <item name> — auto-equip an item from backpack/bags by name
            if (cmdLower == "equip" && spacePos != std::string::npos) {
                std::string equipArg = command.substr(spacePos + 1);
                while (!equipArg.empty() && equipArg.front() == ' ') equipArg.erase(equipArg.begin());
                while (!equipArg.empty() && equipArg.back()  == ' ') equipArg.pop_back();
                std::string equipArgLower = equipArg;
                for (char& c : equipArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                bool found = false;
                const auto& inv = gameHandler.getInventory();
                // Search backpack
                for (int s = 0; s < inv.getBackpackSize() && !found; ++s) {
                    const auto& slot = inv.getBackpackSlot(s);
                    if (slot.empty()) continue;
                    const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                    if (!info) continue;
                    std::string nameLow = info->name;
                    for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (nameLow == equipArgLower) {
                        gameHandler.autoEquipItemBySlot(s);
                        found = true;
                    }
                }
                // Search bags
                for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS && !found; ++b) {
                    for (int s = 0; s < inv.getBagSize(b) && !found; ++s) {
                        const auto& slot = inv.getBagSlot(b, s);
                        if (slot.empty()) continue;
                        const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                        if (!info) continue;
                        std::string nameLow = info->name;
                        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (nameLow == equipArgLower) {
                            gameHandler.autoEquipItemInBag(b, s);
                            found = true;
                        }
                    }
                }
                if (!found) {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "Item not found: '" + equipArg + "'.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Targeting commands
            if (cmdLower == "cleartarget") {
                // Support macro conditionals: /cleartarget [dead] clears only if target is dead
                bool ctCondPass = true;
                if (spacePos != std::string::npos) {
                    std::string ctArg = command.substr(spacePos + 1);
                    while (!ctArg.empty() && ctArg.front() == ' ') ctArg.erase(ctArg.begin());
                    if (!ctArg.empty() && ctArg.front() == '[') {
                        uint64_t ctOver = static_cast<uint64_t>(-1);
                        std::string res = evaluateMacroConditionals(ctArg, gameHandler, ctOver);
                        ctCondPass = !(res.empty() && ctOver == static_cast<uint64_t>(-1));
                    }
                }
                if (ctCondPass) gameHandler.clearTarget();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "target" && spacePos != std::string::npos) {
                // Search visible entities for name match (case-insensitive prefix).
                // Among all matches, pick the nearest living unit to the player.
                // Supports WoW macro conditionals: /target [target=mouseover]; /target [mod:shift] Boss
                std::string targetArg = command.substr(spacePos + 1);

                // Evaluate conditionals if present
                uint64_t targetCmdOverride = static_cast<uint64_t>(-1);
                if (!targetArg.empty() && targetArg.front() == '[') {
                    targetArg = evaluateMacroConditionals(targetArg, gameHandler, targetCmdOverride);
                    if (targetArg.empty() && targetCmdOverride == static_cast<uint64_t>(-1)) {
                        // No condition matched — silently skip (macro fallthrough)
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                    while (!targetArg.empty() && targetArg.front() == ' ') targetArg.erase(targetArg.begin());
                    while (!targetArg.empty() && targetArg.back()  == ' ') targetArg.pop_back();
                }

                // If conditionals resolved to a specific GUID, target it directly
                if (targetCmdOverride != static_cast<uint64_t>(-1) && targetCmdOverride != 0) {
                    gameHandler.setTarget(targetCmdOverride);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                // If no name remains (bare conditional like [target=mouseover] with 0 guid), skip silently
                if (targetArg.empty()) {
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                std::string targetArgLower = targetArg;
                for (char& c : targetArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                uint64_t bestGuid = 0;
                float    bestDist = std::numeric_limits<float>::max();
                const auto& pmi = gameHandler.getMovementInfo();
                const float playerX = pmi.x;
                const float playerY = pmi.y;
                const float playerZ = pmi.z;
                for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                    if (!entity || entity->getType() == game::ObjectType::OBJECT) continue;
                    std::string name;
                    if (entity->getType() == game::ObjectType::PLAYER ||
                        entity->getType() == game::ObjectType::UNIT) {
                        auto unit = std::static_pointer_cast<game::Unit>(entity);
                        name = unit->getName();
                    }
                    if (name.empty()) continue;
                    std::string nameLower = name;
                    for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (nameLower.find(targetArgLower) == 0) {
                        float dx = entity->getX() - playerX;
                        float dy = entity->getY() - playerY;
                        float dz = entity->getZ() - playerZ;
                        float dist = dx*dx + dy*dy + dz*dz;
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestGuid = guid;
                        }
                    }
                }
                if (bestGuid) {
                    gameHandler.setTarget(bestGuid);
                } else {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "No target matching '" + targetArg + "' found.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetenemy") {
                gameHandler.targetEnemy(false);
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetfriend") {
                gameHandler.targetFriend(false);
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetlasttarget" || cmdLower == "targetlast") {
                gameHandler.targetLastTarget();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetlastenemy") {
                gameHandler.targetEnemy(true);  // Reverse direction
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetlastfriend") {
                gameHandler.targetFriend(true);  // Reverse direction
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "focus") {
                // /focus                  → set current target as focus
                // /focus PlayerName       → search for entity by name and set as focus
                // /focus [target=X] Name  → macro conditional: set focus to resolved target
                if (spacePos != std::string::npos) {
                    std::string focusArg = command.substr(spacePos + 1);

                    // Evaluate conditionals if present
                    uint64_t focusCmdOverride = static_cast<uint64_t>(-1);
                    if (!focusArg.empty() && focusArg.front() == '[') {
                        focusArg = evaluateMacroConditionals(focusArg, gameHandler, focusCmdOverride);
                        if (focusArg.empty() && focusCmdOverride == static_cast<uint64_t>(-1)) {
                            chatInputBuffer_[0] = '\0';
                            return;
                        }
                        while (!focusArg.empty() && focusArg.front() == ' ') focusArg.erase(focusArg.begin());
                        while (!focusArg.empty() && focusArg.back()  == ' ') focusArg.pop_back();
                    }

                    if (focusCmdOverride != static_cast<uint64_t>(-1) && focusCmdOverride != 0) {
                        // Conditional resolved to a specific GUID (e.g. [target=mouseover])
                        gameHandler.setFocus(focusCmdOverride);
                    } else if (!focusArg.empty()) {
                        // Name search — same logic as /target
                        std::string focusArgLower = focusArg;
                        for (char& c : focusArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        uint64_t bestGuid = 0;
                        float    bestDist = std::numeric_limits<float>::max();
                        const auto& pmi = gameHandler.getMovementInfo();
                        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                            if (!entity || entity->getType() == game::ObjectType::OBJECT) continue;
                            std::string name;
                            if (entity->getType() == game::ObjectType::PLAYER ||
                                entity->getType() == game::ObjectType::UNIT) {
                                auto unit = std::static_pointer_cast<game::Unit>(entity);
                                name = unit->getName();
                            }
                            if (name.empty()) continue;
                            std::string nameLower = name;
                            for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (nameLower.find(focusArgLower) == 0) {
                                float dx = entity->getX() - pmi.x;
                                float dy = entity->getY() - pmi.y;
                                float dz = entity->getZ() - pmi.z;
                                float dist = dx*dx + dy*dy + dz*dz;
                                if (dist < bestDist) { bestDist = dist; bestGuid = guid; }
                            }
                        }
                        if (bestGuid) {
                            gameHandler.setFocus(bestGuid);
                        } else {
                            game::MessageChatData msg;
                            msg.type = game::ChatType::SYSTEM;
                            msg.language = game::ChatLanguage::UNIVERSAL;
                            msg.message = "No unit matching '" + focusArg + "' found.";
                            gameHandler.addLocalChatMessage(msg);
                        }
                    }
                } else if (gameHandler.hasTarget()) {
                    gameHandler.setFocus(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a unit to set as focus.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clearfocus") {
                gameHandler.clearFocus();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /unstuck command — resets player position to floor height
            if (cmdLower == "unstuck") {
                gameHandler.unstuck();
                chatInputBuffer_[0] = '\0';
                return;
            }
            // /unstuckgy command — move to nearest graveyard
            if (cmdLower == "unstuckgy") {
                gameHandler.unstuckGy();
                chatInputBuffer_[0] = '\0';
                return;
            }
            // /unstuckhearth command — teleport to hearthstone bind point
            if (cmdLower == "unstuckhearth") {
                gameHandler.unstuckHearth();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /transport board — board test transport
            if (cmdLower == "transport board") {
                auto* tm = gameHandler.getTransportManager();
                if (tm) {
                    // Test transport GUID
                    uint64_t testTransportGuid = 0x1000000000000001ULL;
                    // Place player at center of deck (rough estimate)
                    glm::vec3 deckCenter(0.0f, 0.0f, 5.0f);
                    gameHandler.setPlayerOnTransport(testTransportGuid, deckCenter);
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Boarded test transport. Use '/transport leave' to disembark.";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Transport system not available.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /transport leave — disembark from transport
            if (cmdLower == "transport leave") {
                if (gameHandler.isOnTransport()) {
                    gameHandler.clearPlayerTransport();
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Disembarked from transport.";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You are not on a transport.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Chat channel slash commands
            // If used without a message (e.g. just "/s"), switch the chat type dropdown
            bool isChannelCommand = false;
            if (cmdLower == "s" || cmdLower == "say") {
                type = game::ChatType::SAY;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 0;
            } else if (cmdLower == "y" || cmdLower == "yell" || cmdLower == "shout") {
                type = game::ChatType::YELL;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 1;
            } else if (cmdLower == "p" || cmdLower == "party") {
                type = game::ChatType::PARTY;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 2;
            } else if (cmdLower == "g" || cmdLower == "guild") {
                type = game::ChatType::GUILD;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 3;
            } else if (cmdLower == "raid" || cmdLower == "rsay" || cmdLower == "ra") {
                type = game::ChatType::RAID;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 5;
            } else if (cmdLower == "raidwarning" || cmdLower == "rw") {
                type = game::ChatType::RAID_WARNING;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 8;
            } else if (cmdLower == "officer" || cmdLower == "o" || cmdLower == "osay") {
                type = game::ChatType::OFFICER;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 6;
            } else if (cmdLower == "battleground" || cmdLower == "bg") {
                type = game::ChatType::BATTLEGROUND;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 7;
            } else if (cmdLower == "instance" || cmdLower == "i") {
                // Instance chat uses PARTY chat type
                type = game::ChatType::PARTY;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 9;
            } else if (cmdLower == "join") {
                // /join with no args: accept pending BG invite if any
                if (spacePos == std::string::npos && gameHandler.hasPendingBgInvite()) {
                    gameHandler.acceptBattlefield();
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                // /join ChannelName [password]
                if (spacePos != std::string::npos) {
                    std::string rest = command.substr(spacePos + 1);
                    size_t pwStart = rest.find(' ');
                    std::string channelName = (pwStart != std::string::npos) ? rest.substr(0, pwStart) : rest;
                    std::string password = (pwStart != std::string::npos) ? rest.substr(pwStart + 1) : "";
                    gameHandler.joinChannel(channelName, password);
                }
                chatInputBuffer_[0] = '\0';
                return;
            } else if (cmdLower == "leave") {
                // /leave ChannelName
                if (spacePos != std::string::npos) {
                    std::string channelName = command.substr(spacePos + 1);
                    gameHandler.leaveChannel(channelName);
                }
                chatInputBuffer_[0] = '\0';
                return;
            } else if ((cmdLower == "wts" || cmdLower == "wtb") && spacePos != std::string::npos) {
                // /wts and /wtb — send to Trade channel
                // Prefix with [WTS] / [WTB] and route to the Trade channel
                const std::string tag = (cmdLower == "wts") ? "[WTS] " : "[WTB] ";
                const std::string body = command.substr(spacePos + 1);
                // Find the Trade channel among joined channels (case-insensitive prefix match)
                std::string tradeChan;
                for (const auto& ch : gameHandler.getJoinedChannels()) {
                    std::string chLow = ch;
                    for (char& c : chLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (chLow.rfind("trade", 0) == 0) { tradeChan = ch; break; }
                }
                if (tradeChan.empty()) {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.language = game::ChatLanguage::UNIVERSAL;
                    errMsg.message = "You are not in the Trade channel.";
                    gameHandler.addLocalChatMessage(errMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                message = tag + body;
                type = game::ChatType::CHANNEL;
                target = tradeChan;
                isChannelCommand = true;
            } else if (cmdLower.size() == 1 && cmdLower[0] >= '1' && cmdLower[0] <= '9') {
                // /1 msg, /2 msg — channel shortcuts
                int channelIdx = cmdLower[0] - '0';
                std::string channelName = gameHandler.getChannelByIndex(channelIdx);
                if (!channelName.empty() && spacePos != std::string::npos) {
                    message = command.substr(spacePos + 1);
                    type = game::ChatType::CHANNEL;
                    target = channelName;
                    isChannelCommand = true;
                } else if (channelName.empty()) {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.message = "You are not in channel " + std::to_string(channelIdx) + ".";
                    gameHandler.addLocalChatMessage(errMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                } else {
                    chatInputBuffer_[0] = '\0';
                    return;
                }
            } else if (cmdLower == "w" || cmdLower == "whisper" || cmdLower == "tell" || cmdLower == "t") {
                switchChatType = 4;
                if (spacePos != std::string::npos) {
                    std::string rest = command.substr(spacePos + 1);
                    size_t msgStart = rest.find(' ');
                    if (msgStart != std::string::npos) {
                        // /w PlayerName message — send whisper immediately
                        target = rest.substr(0, msgStart);
                        message = rest.substr(msgStart + 1);
                        type = game::ChatType::WHISPER;
                        isChannelCommand = true;
                        // Set whisper target for future messages
                        strncpy(whisperTargetBuffer_, target.c_str(), sizeof(whisperTargetBuffer_) - 1);
                        whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                    } else {
                        // /w PlayerName — switch to whisper mode with target set
                        strncpy(whisperTargetBuffer_, rest.c_str(), sizeof(whisperTargetBuffer_) - 1);
                        whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                        message = "";
                        isChannelCommand = true;
                    }
                } else {
                    // Just "/w" — switch to whisper mode
                    message = "";
                    isChannelCommand = true;
                }
            } else if (cmdLower == "r" || cmdLower == "reply") {
                switchChatType = 4;
                std::string lastSender = gameHandler.getLastWhisperSender();
                if (lastSender.empty()) {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "No one has whispered you yet.";
                    gameHandler.addLocalChatMessage(sysMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                target = lastSender;
                strncpy(whisperTargetBuffer_, target.c_str(), sizeof(whisperTargetBuffer_) - 1);
                whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                if (spacePos != std::string::npos) {
                    message = command.substr(spacePos + 1);
                    type = game::ChatType::WHISPER;
                } else {
                    message = "";
                }
                isChannelCommand = true;
            }

            // Check for emote commands
            if (!isChannelCommand) {
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
                    // Play the emote animation
                    auto* renderer = services_.renderer;
                    if (renderer) {
                        if (auto* ac = renderer->getAnimationController()) ac->playEmote(cmdLower);
                    }

                    // Send CMSG_TEXT_EMOTE to server
                    uint32_t dbcId = rendering::AnimationController::getEmoteDbcId(cmdLower);
                    if (dbcId != 0) {
                        uint64_t targetGuid = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                        gameHandler.sendTextEmote(dbcId, targetGuid);
                    }

                    // Add local chat message
                    game::MessageChatData msg;
                    msg.type = game::ChatType::TEXT_EMOTE;
                    msg.language = game::ChatLanguage::COMMON;
                    msg.message = emoteText;
                    gameHandler.addLocalChatMessage(msg);

                    chatInputBuffer_[0] = '\0';
                    return;
                }

                // Not a recognized command — fall through and send as normal chat
                if (!isChannelCommand) {
                    message = input;
                }
            }

            // If no valid command found and starts with /, just send as-is
            if (!isChannelCommand && message == input) {
                // Use the selected chat type from dropdown
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
            }
        } else {
            // No slash command, use the selected chat type from dropdown
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
        }

        // Whisper shortcuts to PortBot/GMBot: translate to GM teleport commands.
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

        // Don't send empty messages — but switch chat type if a channel shortcut was used
        if (!message.empty()) {
            gameHandler.sendChatMessage(type, message, target);
        }

        // Switch chat type dropdown when channel shortcut used (with or without message)
        if (switchChatType >= 0) {
            selectedChatType_ = switchChatType;
        }

        // Clear input
        chatInputBuffer_[0] = '\0';
    }
}



} // namespace ui
} // namespace wowee
