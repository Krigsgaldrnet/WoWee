// Forward declarations for the chat subsystem.
// Include this instead of the full headers when only pointers/references are needed.
// Extracted in Phase 6.6 of chat_panel_ref.md.
#pragma once

namespace wowee {

namespace game { class GameHandler; }

namespace ui {

class ChatPanel;
class InventoryScreen;
class SpellbookScreen;
class QuestLogScreen;

// Chat subsystem types (under include/ui/chat/)
class ChatSettings;
class ChatInput;
class ChatTabManager;
class ChatBubbleManager;
class ChatMarkupParser;
class ChatMarkupRenderer;
class ChatCommandRegistry;
class ChatTabCompleter;
class CastSequenceTracker;
class MacroEvaluator;
class IGameState;
class IModifierState;
class IChatCommand;

struct ChatCommandContext;
struct ChatCommandResult;
struct UIServices;

} // namespace ui
} // namespace wowee
