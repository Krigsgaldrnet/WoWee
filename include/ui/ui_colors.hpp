#pragma once

#include <imgui.h>
#include "game/inventory.hpp"

namespace wowee::ui {

// ---- Common UI colors ----
namespace colors {
    constexpr ImVec4 kRed         = {1.0f, 0.3f, 0.3f, 1.0f};
    constexpr ImVec4 kGreen       = {0.4f, 1.0f, 0.4f, 1.0f};
    constexpr ImVec4 kBrightGreen = {0.3f, 1.0f, 0.3f, 1.0f};
    constexpr ImVec4 kYellow      = {1.0f, 1.0f, 0.3f, 1.0f};
    constexpr ImVec4 kGray        = {0.6f, 0.6f, 0.6f, 1.0f};
    constexpr ImVec4 kDarkGray    = {0.5f, 0.5f, 0.5f, 1.0f};
    constexpr ImVec4 kLightGray   = {0.7f, 0.7f, 0.7f, 1.0f};
    constexpr ImVec4 kWhite       = {1.0f, 1.0f, 1.0f, 1.0f};

    // Coin colors
    constexpr ImVec4 kGold   = {1.00f, 0.82f, 0.00f, 1.0f};
    constexpr ImVec4 kSilver = {0.80f, 0.80f, 0.80f, 1.0f};
    constexpr ImVec4 kCopper = {0.72f, 0.45f, 0.20f, 1.0f};
} // namespace colors

// ---- Item quality colors ----
inline ImVec4 getQualityColor(game::ItemQuality quality) {
    switch (quality) {
        case game::ItemQuality::POOR:      return {0.62f, 0.62f, 0.62f, 1.0f};
        case game::ItemQuality::COMMON:    return {1.0f, 1.0f, 1.0f, 1.0f};
        case game::ItemQuality::UNCOMMON:  return {0.12f, 1.0f, 0.0f, 1.0f};
        case game::ItemQuality::RARE:      return {0.0f, 0.44f, 0.87f, 1.0f};
        case game::ItemQuality::EPIC:      return {0.64f, 0.21f, 0.93f, 1.0f};
        case game::ItemQuality::LEGENDARY: return {1.0f, 0.50f, 0.0f, 1.0f};
        case game::ItemQuality::ARTIFACT:  return {0.90f, 0.80f, 0.50f, 1.0f};
        case game::ItemQuality::HEIRLOOM:  return {0.90f, 0.80f, 0.50f, 1.0f};
        default:                           return {1.0f, 1.0f, 1.0f, 1.0f};
    }
}

// ---- Coin display (gold/silver/copper) ----
inline void renderCoinsText(uint32_t g, uint32_t s, uint32_t c) {
    bool any = false;
    if (g > 0) {
        ImGui::TextColored(colors::kGold, "%ug", g);
        any = true;
    }
    if (s > 0 || g > 0) {
        if (any) ImGui::SameLine(0, 3);
        ImGui::TextColored(colors::kSilver, "%us", s);
        any = true;
    }
    if (any) ImGui::SameLine(0, 3);
    ImGui::TextColored(colors::kCopper, "%uc", c);
}

// Convenience overload: decompose copper amount and render as gold/silver/copper
inline void renderCoinsFromCopper(uint64_t copper) {
    renderCoinsText(static_cast<uint32_t>(copper / 10000),
                    static_cast<uint32_t>((copper / 100) % 100),
                    static_cast<uint32_t>(copper % 100));
}

} // namespace wowee::ui
