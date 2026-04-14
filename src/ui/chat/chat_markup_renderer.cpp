// ChatMarkupRenderer — render parsed ChatSegments via ImGui.
// Moved from ChatPanel::render() inline lambdas (Phase 2.2).
// Item tooltip rendering extracted to ItemTooltipRenderer (Phase 6.7).
#include "ui/chat/chat_markup_renderer.hpp"
#include "ui/chat/item_tooltip_renderer.hpp"
#include "ui/ui_colors.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/quest_log_screen.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include <imgui.h>
#include <cstring>

namespace wowee { namespace ui {

// ---- Main segment renderer ----

void ChatMarkupRenderer::render(
    const std::vector<ChatSegment>& segments,
    const ImVec4& baseColor,
    const MarkupRenderContext& ctx) const
{
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];
        bool needSameLine = (i + 1 < segments.size());

        switch (seg.type) {
        case SegmentType::Text: {
            if (!seg.text.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, baseColor);
                ImGui::TextWrapped("%s", seg.text.c_str());
                ImGui::PopStyleColor();
                if (needSameLine) ImGui::SameLine(0, 0);
            }
            break;
        }
        case SegmentType::ColoredText: {
            if (!seg.text.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, seg.color);
                ImGui::TextWrapped("%s", seg.text.c_str());
                ImGui::PopStyleColor();
                if (needSameLine) ImGui::SameLine(0, 0);
            }
            break;
        }
        case SegmentType::ItemLink: {
            uint32_t itemEntry = seg.id;
            if (itemEntry > 0 && ctx.gameHandler) {
                ctx.gameHandler->ensureItemInfo(itemEntry);
            }
            // Show small icon before item link if available
            if (itemEntry > 0 && ctx.gameHandler && ctx.inventory) {
                const auto* chatInfo = ctx.gameHandler->getItemInfo(itemEntry);
                if (chatInfo && chatInfo->valid && chatInfo->displayInfoId != 0) {
                    VkDescriptorSet chatIcon = ctx.inventory->getItemIcon(chatInfo->displayInfoId);
                    if (chatIcon) {
                        ImGui::Image((ImTextureID)(uintptr_t)chatIcon, ImVec2(12, 12));
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ItemTooltipRenderer::render(itemEntry, *ctx.gameHandler, *ctx.inventory, ctx.assetMgr);
                        }
                        ImGui::SameLine(0, 2);
                    }
                }
            }
            std::string display = "[" + seg.text + "]";
            ImGui::PushStyleColor(ImGuiCol_Text, seg.color);
            ImGui::TextWrapped("%s", display.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (itemEntry > 0 && ctx.gameHandler && ctx.inventory) {
                    ItemTooltipRenderer::render(itemEntry, *ctx.gameHandler, *ctx.inventory, ctx.assetMgr);
                }
            }
            // Shift-click: insert entire link back into chat input
            if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift &&
                ctx.chatInputBuffer && ctx.moveCursorToEnd) {
                size_t curLen = strlen(ctx.chatInputBuffer);
                if (curLen + seg.rawLink.size() + 1 < ctx.chatInputBufSize) {
                    strncat(ctx.chatInputBuffer, seg.rawLink.c_str(), ctx.chatInputBufSize - curLen - 1);
                    *ctx.moveCursorToEnd = true;
                }
            }
            if (needSameLine) ImGui::SameLine(0, 0);
            break;
        }
        case SegmentType::SpellLink: {
            // Small icon (use spell icon cache if available)
            VkDescriptorSet spellIcon = VK_NULL_HANDLE;
            if (seg.id > 0 && ctx.getSpellIcon && ctx.assetMgr) {
                spellIcon = ctx.getSpellIcon(seg.id, ctx.assetMgr);
            }
            if (spellIcon) {
                ImGui::Image((ImTextureID)(uintptr_t)spellIcon, ImVec2(12, 12));
                if (ImGui::IsItemHovered() && ctx.spellbook && ctx.gameHandler && ctx.assetMgr) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ctx.spellbook->renderSpellInfoTooltip(seg.id, *ctx.gameHandler, ctx.assetMgr);
                }
                ImGui::SameLine(0, 2);
            }
            std::string display = "[" + seg.text + "]";
            ImGui::PushStyleColor(ImGuiCol_Text, seg.color);
            ImGui::TextWrapped("%s", display.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ctx.spellbook && ctx.gameHandler && ctx.assetMgr) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ctx.spellbook->renderSpellInfoTooltip(seg.id, *ctx.gameHandler, ctx.assetMgr);
            }
            // Shift-click: insert link
            if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift &&
                ctx.chatInputBuffer && ctx.moveCursorToEnd) {
                size_t curLen = strlen(ctx.chatInputBuffer);
                if (curLen + seg.rawLink.size() + 1 < ctx.chatInputBufSize) {
                    strncat(ctx.chatInputBuffer, seg.rawLink.c_str(), ctx.chatInputBufSize - curLen - 1);
                    *ctx.moveCursorToEnd = true;
                }
            }
            if (needSameLine) ImGui::SameLine(0, 0);
            break;
        }
        case SegmentType::QuestLink: {
            std::string display = "[" + seg.text + "]";
            ImGui::PushStyleColor(ImGuiCol_Text, colors::kWarmGold);
            ImGui::TextWrapped("%s", display.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::BeginTooltip();
                ImGui::TextColored(colors::kWarmGold, "%s", seg.text.c_str());
                if (seg.extra > 0) ImGui::TextDisabled("Level %u Quest", seg.extra);
                ImGui::TextDisabled("Click quest log to view details");
                ImGui::EndTooltip();
            }
            if (ImGui::IsItemClicked() && seg.id > 0 && ctx.questLog) {
                ctx.questLog->openAndSelectQuest(seg.id);
            }
            // Shift-click: insert link
            if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift &&
                ctx.chatInputBuffer && ctx.moveCursorToEnd) {
                size_t curLen = strlen(ctx.chatInputBuffer);
                if (curLen + seg.rawLink.size() + 1 < ctx.chatInputBufSize) {
                    strncat(ctx.chatInputBuffer, seg.rawLink.c_str(), ctx.chatInputBufSize - curLen - 1);
                    *ctx.moveCursorToEnd = true;
                }
            }
            if (needSameLine) ImGui::SameLine(0, 0);
            break;
        }
        case SegmentType::AchievementLink: {
            std::string display = "[" + seg.text + "]";
            ImGui::PushStyleColor(ImGuiCol_Text, colors::kBrightGold);
            ImGui::TextWrapped("%s", display.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Achievement: %s", seg.text.c_str());
            }
            // Shift-click: insert link
            if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift &&
                ctx.chatInputBuffer && ctx.moveCursorToEnd) {
                size_t curLen = strlen(ctx.chatInputBuffer);
                if (curLen + seg.rawLink.size() + 1 < ctx.chatInputBufSize) {
                    strncat(ctx.chatInputBuffer, seg.rawLink.c_str(), ctx.chatInputBufSize - curLen - 1);
                    *ctx.moveCursorToEnd = true;
                }
            }
            if (needSameLine) ImGui::SameLine(0, 0);
            break;
        }
        case SegmentType::Url: {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
            ImGui::TextWrapped("%s", seg.text.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Open: %s", seg.text.c_str());
            }
            if (ImGui::IsItemClicked()) {
                std::string cmd = "xdg-open '" + seg.text + "' &";
                [[maybe_unused]] int result = system(cmd.c_str());
            }
            ImGui::PopStyleColor();
            if (needSameLine) ImGui::SameLine(0, 0);
            break;
        }
        } // switch
    }
}

} // namespace ui
} // namespace wowee
