#pragma once

#include <cstdint>

namespace space_scanner::gui
{

    struct RenderPoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct RenderRect
    {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    struct RenderVisualPalette
    {
        std::uint8_t window_background_red = 0;
        std::uint8_t window_background_green = 0;
        std::uint8_t window_background_blue = 0;
        std::uint8_t header_panel_red = 0;
        std::uint8_t header_panel_green = 0;
        std::uint8_t header_panel_blue = 0;
        std::uint8_t control_panel_red = 0;
        std::uint8_t control_panel_green = 0;
        std::uint8_t control_panel_blue = 0;
        std::uint8_t control_surface_red = 0;
        std::uint8_t control_surface_green = 0;
        std::uint8_t control_surface_blue = 0;
        std::uint8_t control_muted_red = 0;
        std::uint8_t control_muted_green = 0;
        std::uint8_t control_muted_blue = 0;
        std::uint8_t control_border_red = 0;
        std::uint8_t control_border_green = 0;
        std::uint8_t control_border_blue = 0;
    };

    constexpr RenderPoint MakeRenderPoint(float x, float y)
    {
        return RenderPoint{ x, y };
    }

    constexpr RenderRect MakeRenderRect(float left, float top, float right, float bottom)
    {
        return RenderRect{ left, top, right, bottom };
    }

} // namespace space_scanner::gui
