#include "gui/direct2d_renderer.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <memory>
#include <sstream>
#include <utility>

namespace swift_disk::gui
{


    struct RenderTextMetrics
    {
        float width_including_trailing_whitespace = 0.0f;
        float height = 0.0f;
    };

    struct HoverTooltipLayout
    {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> name_layout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> size_layout;
        RenderRect shadow_rect = MakeRenderRect(0, 0, 0, 0);
        RenderRect tooltip_rect = MakeRenderRect(0, 0, 0, 0);
        RenderPoint name_origin = MakeRenderPoint(0, 0);
        RenderPoint size_origin = MakeRenderPoint(0, 0);
    };


    enum class RenderTextFormat
    {
        Title,
        Body,
        Small
    };

    class Direct2DRenderer
    {
    public:
        Direct2DRenderer() = default;
        Direct2DRenderer(const Direct2DRenderer&) = delete;
        Direct2DRenderer& operator=(const Direct2DRenderer&) = delete;

        HRESULT CreateDeviceResources(HWND hwnd, const RenderVisualPalette& palette);
        void DiscardDeviceResources();
        bool HasRenderTarget() const;
        HRESULT Resize(HWND hwnd);
        void Paint(HWND hwnd, const MainWindowDrawingState& state);
        RenderRect GetHoverTooltipBounds(HWND hwnd, const MainWindowDrawingState& state);
        void BeginDraw();
        void SetIdentityTransform();
        void Clear(const D2D1_COLOR_F& color);
        bool EndDrawNeedsRecreate();
        void PushClip(const RenderRect& rect);
        void PopClip();
        void FillRectangle(const RenderRect& rect, ID2D1SolidColorBrush* brush);
        void DrawRectangle(const RenderRect& rect, ID2D1SolidColorBrush* brush, float stroke_width = 1.0f);
        void FillRoundedRectangle(const RenderRect& rect, float radius_x, float radius_y, ID2D1SolidColorBrush* brush);
        void DrawRoundedRectangle(const RenderRect& rect, float radius_x, float radius_y, ID2D1SolidColorBrush* brush, float stroke_width = 1.0f);
        void DrawLine(const RenderPoint& start, const RenderPoint& end, ID2D1SolidColorBrush* brush, float stroke_width = 1.0f);
        void DrawTextLine(const std::wstring& text, RenderTextFormat format, const RenderRect& bounds, ID2D1SolidColorBrush* brush);
        void DrawTileTextLayout(
            const std::wstring& text,
            RenderTextFormat format,
            const RenderRect& bounds,
            ID2D1SolidColorBrush* text_brush,
            ID2D1SolidColorBrush* outline_brush,
            DWRITE_TEXT_ALIGNMENT text_alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
            DWRITE_WORD_WRAPPING word_wrapping = DWRITE_WORD_WRAPPING_NO_WRAP);
        void DrawTileTextLine(
            const std::wstring& text,
            RenderTextFormat format,
            const RenderRect& bounds,
            ID2D1SolidColorBrush* text_brush,
            ID2D1SolidColorBrush* outline_brush,
            DWRITE_TEXT_ALIGNMENT text_alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
        float MeasureTextWidth(const std::wstring& text, RenderTextFormat format) const;
        RenderTextMetrics MeasureTextLayout(const std::wstring& text, RenderTextFormat format, float max_width, float max_height, DWRITE_WORD_WRAPPING word_wrapping) const;
        void DrawTextLayoutAt(const std::wstring& text, RenderTextFormat format, float max_width, float max_height, DWRITE_WORD_WRAPPING word_wrapping, const RenderPoint& origin, ID2D1SolidColorBrush* brush);

    private:
        std::wstring BuildDirectoryHeaderText(const std::wstring& label, std::uint64_t size, float available_width) const;
        void DrawPanelRect(const RenderRect& rect, ID2D1SolidColorBrush* fill_brush, ID2D1SolidColorBrush* border_brush, float stroke_width = 1.0f);
        void DrawHeaderGroupBox(const RenderRect& rect, const std::wstring& label);
        void DrawTreemap(const MainWindowDrawingState& state);
        void DrawHeader(const MainWindowDrawingState& state);
        RenderRect GetHoveredRectBounds(const MainWindowDrawingState& state, const visualization::TreemapRect& rect) const;
        bool TryGetHoverDisplayInfo(
            const MainWindowDrawingState& state,
            std::wstring* name_text,
            std::wstring* size_text,
            RenderRect* hovered_bounds,
            bool* follow_tooltip) const;
        RenderPoint ResolveTooltipPosition(
            const MainWindowDrawingState& state,
            const RenderPoint& anchor,
            const RenderRect& hovered_rect,
            float tooltip_width,
            float tooltip_height,
            bool follow_tooltip) const;
        bool BuildHoverTooltipLayout(const MainWindowDrawingState& state, HoverTooltipLayout* layout) const;
        void DrawHoverTooltip(const MainWindowDrawingState& state);
        void DrawStatusBar(const MainWindowDrawingState& state);

        HRESULT CreateBrush(Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& color);
        HRESULT CreateTextFormats();
        void ApplyTrimming(IDWriteTextLayout* layout, DWRITE_WORD_WRAPPING word_wrapping) const;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> CreateTextLayout(const std::wstring& text, RenderTextFormat format, const RenderRect& bounds, DWRITE_WORD_WRAPPING word_wrapping) const;
        IDWriteTextFormat* GetFormat(RenderTextFormat format) const;
        float Scale(float value) const;

        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d_factory;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite_factory;
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_render_target;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_window_background_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_text_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_muted_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_panel_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_canvas_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_border_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_separator_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tile_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tile_text_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tile_secondary_text_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tile_text_outline_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tile_text_plate_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_hover_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_active_level_glow_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_active_level_shadow_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_error_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_header_action_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tooltip_background_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tooltip_border_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tooltip_text_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tooltip_secondary_text_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tooltip_shadow_brush;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_title_format;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_body_format;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_small_format;
        Microsoft::WRL::ComPtr<IDWriteInlineObject> m_ellipsis_trimming_sign;
        float m_ui_scale = 1.0f;
    };

    using Microsoft::WRL::ComPtr;

    //Use unnamed namespace to aviod external linkage for local helper functions.
    namespace
    {

    D2D1_POINT_2F ToD2DPoint(const RenderPoint& point)
    {
        return D2D1::Point2F(point.x, point.y);
    }

    D2D1_RECT_F ToD2DRect(const RenderRect& rect)
    {
        return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
    }

    D2D1_RENDER_TARGET_PROPERTIES CreatePixelRenderTargetProperties()
    {
        return D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
            96.0f,
            96.0f);
    }

    float GetWindowDpiScale(HWND hwnd)
    {
        if (!hwnd)
        {
            return 1.0f;
        }

        HDC hdc = GetDC(hwnd);
        if (!hdc)
        {
            return 1.0f;
        }

        const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
        return std::clamp(static_cast<float>(dpi) / 96.0f, 1.0f, 4.0f);
    }

    bool IsRenderRectEmpty(const RenderRect& rect)
    {
        return rect.left >= rect.right || rect.top >= rect.bottom;
    }

    bool RenderRectsIntersect(const RenderRect& left, const RenderRect& right)
    {
        if (IsRenderRectEmpty(left) || IsRenderRectEmpty(right))
        {
            return false;
        }

        return left.left < right.right &&
            left.right > right.left &&
            left.top < right.bottom &&
            left.bottom > right.top;
    }

    RenderRect InflateRenderRect(const RenderRect& rect, float amount)
    {
        return MakeRenderRect(
            rect.left - amount,
            rect.top - amount,
            rect.right + amount,
            rect.bottom + amount);
    }

    D2D1_COLOR_F MakeD2DColor(std::uint8_t red, std::uint8_t green, std::uint8_t blue, float alpha = 1.0f)
    {
        return D2D1::ColorF(
            static_cast<float>(red) / 255.0f,
            static_cast<float>(green) / 255.0f,
            static_cast<float>(blue) / 255.0f,
            alpha);
    }

    std::wstring HumanizeBytes(std::uint64_t bytes)
    {
        static constexpr const wchar_t* kUnits[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB" };
        double value = static_cast<double>(bytes);
        size_t unit_index = 0;
        while (value >= 1024.0 && unit_index + 1 < std::size(kUnits))
        {
            value /= 1024.0;
            ++unit_index;
        }

        wchar_t buffer[64] = {};
        swprintf_s(buffer, L"%.2f %s", value, kUnits[unit_index]);
        return buffer;
    }

    std::wstring FormatSeconds(double seconds)
    {
        wchar_t buffer[64] = {};
        swprintf_s(buffer, L"%.2f s", seconds);
        return buffer;
    }

    D2D1_COLOR_F ParseHexColor(const std::string& value, float alpha = 1.0f)
    {
        if (value.size() != 7 || value[0] != '#')
        {
            return MakeD2DColor(120, 150, 190, alpha);
        }

        auto hex = [](char ch) -> std::uint8_t
        {
            if (ch >= '0' && ch <= '9')
            {
                return static_cast<std::uint8_t>(ch - '0');
            }
            if (ch >= 'a' && ch <= 'f')
            {
                return static_cast<std::uint8_t>(10 + ch - 'a');
            }
            if (ch >= 'A' && ch <= 'F')
            {
                return static_cast<std::uint8_t>(10 + ch - 'A');
            }
            return 0;
        };

        const std::uint8_t red = static_cast<std::uint8_t>((hex(value[1]) << 4) | hex(value[2]));
        const std::uint8_t green = static_cast<std::uint8_t>((hex(value[3]) << 4) | hex(value[4]));
        const std::uint8_t blue = static_cast<std::uint8_t>((hex(value[5]) << 4) | hex(value[6]));
        return MakeD2DColor(red, green, blue, alpha);
    }

    }

    HRESULT Direct2DRenderer::CreateDeviceResources(HWND hwnd, const RenderVisualPalette& palette)
    {
        if (!m_d2d_factory)
        {
            HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d_factory.GetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }
        }

        if (!m_dwrite_factory)
        {
            HRESULT hr = DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwrite_factory.GetAddressOf()));
            if (FAILED(hr))
            {
                return hr;
            }
        }

        const float window_scale = GetWindowDpiScale(hwnd);
        if (std::fabs(window_scale - m_ui_scale) > 0.001f)
        {
            m_ui_scale = window_scale;
            m_title_format.Reset();
            m_body_format.Reset();
            m_small_format.Reset();
            m_ellipsis_trimming_sign.Reset();
        }

        if (!m_render_target)
        {
            RECT client = {};
            GetClientRect(hwnd, &client);
            const D2D1_SIZE_U size = D2D1::SizeU(
                static_cast<UINT>(std::max<LONG>(client.right - client.left, 1)),
                static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1)));

            HRESULT hr = m_d2d_factory->CreateHwndRenderTarget(
                CreatePixelRenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(hwnd, size),
                m_render_target.GetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            hr = CreateBrush(
                m_window_background_brush,
                MakeD2DColor(
                    palette.window_background_red,
                    palette.window_background_green,
                    palette.window_background_blue));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_text_brush, MakeD2DColor(232, 241, 255));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(
                m_muted_brush,
                MakeD2DColor(
                    palette.control_muted_red,
                    palette.control_muted_green,
                    palette.control_muted_blue));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(
                m_panel_brush,
                MakeD2DColor(
                    palette.header_panel_red,
                    palette.header_panel_green,
                    palette.header_panel_blue));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(
                m_canvas_brush,
                MakeD2DColor(
                    palette.control_panel_red,
                    palette.control_panel_green,
                    palette.control_panel_blue));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(
                m_border_brush,
                MakeD2DColor(
                    palette.control_border_red,
                    palette.control_border_green,
                    palette.control_border_blue,
                    0.78f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_separator_brush, MakeD2DColor(142, 154, 171, 0.92f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tile_brush, MakeD2DColor(120, 160, 215));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tile_text_brush, MakeD2DColor(232, 241, 255));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tile_secondary_text_brush, MakeD2DColor(150, 171, 204));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tile_text_outline_brush, MakeD2DColor(8, 17, 29, 0.72f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tile_text_plate_brush, MakeD2DColor(8, 17, 29, 0.36f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_hover_brush, MakeD2DColor(250, 252, 255, 0.95f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_active_level_glow_brush, MakeD2DColor(150, 205, 255, 0.24f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_active_level_shadow_brush, MakeD2DColor(6, 10, 18, 0.48f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_error_brush, MakeD2DColor(255, 140, 140));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(
                m_header_action_brush,
                MakeD2DColor(
                    palette.control_surface_red,
                    palette.control_surface_green,
                    palette.control_surface_blue,
                    0.82f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tooltip_background_brush, MakeD2DColor(255, 255, 255, 0.98f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tooltip_border_brush, MakeD2DColor(166, 172, 181, 0.95f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tooltip_text_brush, MakeD2DColor(42, 42, 42, 0.96f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tooltip_secondary_text_brush, MakeD2DColor(86, 86, 86, 0.96f));
            if (FAILED(hr))
            {
                return hr;
            }
            hr = CreateBrush(m_tooltip_shadow_brush, MakeD2DColor(0, 0, 0, 0.18f));
            if (FAILED(hr))
            {
                return hr;
            }
        }

        if (!m_title_format || !m_body_format || !m_small_format || !m_ellipsis_trimming_sign)
        {
            HRESULT hr = CreateTextFormats();
            if (FAILED(hr))
            {
                return hr;
            }
        }

        return S_OK;
    }

    void Direct2DRenderer::DiscardDeviceResources()
    {
        m_render_target.Reset();
        m_window_background_brush.Reset();
        m_text_brush.Reset();
        m_muted_brush.Reset();
        m_panel_brush.Reset();
        m_canvas_brush.Reset();
        m_border_brush.Reset();
        m_separator_brush.Reset();
        m_tile_brush.Reset();
        m_tile_text_brush.Reset();
        m_tile_secondary_text_brush.Reset();
        m_tile_text_outline_brush.Reset();
        m_tile_text_plate_brush.Reset();
        m_hover_brush.Reset();
        m_active_level_glow_brush.Reset();
        m_active_level_shadow_brush.Reset();
        m_error_brush.Reset();
        m_header_action_brush.Reset();
        m_tooltip_background_brush.Reset();
        m_tooltip_border_brush.Reset();
        m_tooltip_text_brush.Reset();
        m_tooltip_secondary_text_brush.Reset();
        m_tooltip_shadow_brush.Reset();
        m_title_format.Reset();
        m_body_format.Reset();
        m_small_format.Reset();
        m_ellipsis_trimming_sign.Reset();
    }

    bool Direct2DRenderer::HasRenderTarget() const
    {
        return m_render_target != nullptr;
    }

    HRESULT Direct2DRenderer::Resize(HWND hwnd)
    {
        if (!m_render_target)
        {
            return S_OK;
        }

        RECT client = {};
        GetClientRect(hwnd, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT>(std::max<LONG>(client.right - client.left, 1)),
            static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1)));
        return m_render_target->Resize(size);
    }

    void Direct2DRenderer::BeginDraw()
    {
        if (m_render_target)
        {
            m_render_target->BeginDraw();
        }
    }

    void Direct2DRenderer::SetIdentityTransform()
    {
        if (m_render_target)
        {
            m_render_target->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    void Direct2DRenderer::Clear(const D2D1_COLOR_F& color)
    {
        if (m_render_target)
        {
            m_render_target->Clear(color);
        }
    }

    void Direct2DRenderer::PushClip(const RenderRect& rect)
    {
        if (m_render_target && !IsRenderRectEmpty(rect))
        {
            m_render_target->PushAxisAlignedClip(
                ToD2DRect(rect),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
    }

    void Direct2DRenderer::PopClip()
    {
        if (m_render_target)
        {
            m_render_target->PopAxisAlignedClip();
        }
    }

    bool Direct2DRenderer::EndDrawNeedsRecreate()
    {
        if (!m_render_target)
        {
            return false;
        }

        return m_render_target->EndDraw() == D2DERR_RECREATE_TARGET;
    }


    void Direct2DRenderer::FillRectangle(const RenderRect& rect, ID2D1SolidColorBrush* brush)
    {
        if (m_render_target && brush)
        {
            m_render_target->FillRectangle(ToD2DRect(rect), brush);
        }
    }

    void Direct2DRenderer::DrawRectangle(const RenderRect& rect, ID2D1SolidColorBrush* brush, float stroke_width)
    {
        if (m_render_target && brush)
        {
            m_render_target->DrawRectangle(ToD2DRect(rect), brush, stroke_width);
        }
    }

    void Direct2DRenderer::FillRoundedRectangle(const RenderRect& rect, float radius_x, float radius_y, ID2D1SolidColorBrush* brush)
    {
        ID2D1SolidColorBrush* fill_brush = brush;
        if (m_render_target && fill_brush)
        {
            m_render_target->FillRoundedRectangle(
                D2D1::RoundedRect(ToD2DRect(rect), radius_x, radius_y),
                fill_brush);
        }
    }

    void Direct2DRenderer::DrawRoundedRectangle(
        const RenderRect& rect,
        float radius_x,
        float radius_y,
        ID2D1SolidColorBrush* brush,
        float stroke_width)
    {
        ID2D1SolidColorBrush* stroke_brush = brush;
        if (m_render_target && stroke_brush)
        {
            m_render_target->DrawRoundedRectangle(
                D2D1::RoundedRect(ToD2DRect(rect), radius_x, radius_y),
                stroke_brush,
                stroke_width);
        }
    }

    void Direct2DRenderer::DrawLine(const RenderPoint& start, const RenderPoint& end, ID2D1SolidColorBrush* brush, float stroke_width)
    {
        ID2D1SolidColorBrush* stroke_brush = brush;
        if (m_render_target && stroke_brush)
        {
            m_render_target->DrawLine(ToD2DPoint(start), ToD2DPoint(end), stroke_brush, stroke_width);
        }
    }

    void Direct2DRenderer::DrawTextLine(
        const std::wstring& text,
        RenderTextFormat format,
        const RenderRect& bounds,
        ID2D1SolidColorBrush* brush)
    {
        IDWriteTextFormat* text_format = GetFormat(format);
        ID2D1SolidColorBrush* text_brush = brush;
        if (!m_render_target || !text_format || !text_brush)
        {
            return;
        }

        m_render_target->DrawTextW(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            text_format,
            ToD2DRect(bounds),
            text_brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Direct2DRenderer::DrawTileTextLayout(
        const std::wstring& text,
        RenderTextFormat format,
        const RenderRect& bounds,
        ID2D1SolidColorBrush* text_brush,
        ID2D1SolidColorBrush* outline_brush,
        DWRITE_TEXT_ALIGNMENT text_alignment,
        DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment,
        DWRITE_WORD_WRAPPING word_wrapping)
    {
        ID2D1SolidColorBrush* foreground_brush = text_brush;
        ID2D1SolidColorBrush* shadow_brush = outline_brush;
        if (!m_render_target || !foreground_brush || !shadow_brush || !m_dwrite_factory)
        {
            return;
        }

        ComPtr<IDWriteTextLayout> layout = CreateTextLayout(text, format, bounds, word_wrapping);
        if (!layout)
        {
            return;
        }

        layout->SetTextAlignment(text_alignment);
        layout->SetParagraphAlignment(paragraph_alignment);
        ApplyTrimming(layout.Get(), word_wrapping);

        const D2D1_POINT_2F shadow_origin = D2D1::Point2F(bounds.left + 1.0f, bounds.top + 1.0f);
        const D2D1_POINT_2F origin = D2D1::Point2F(bounds.left, bounds.top);
        m_render_target->DrawTextLayout(shadow_origin, layout.Get(), shadow_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        m_render_target->DrawTextLayout(origin, layout.Get(), foreground_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Direct2DRenderer::DrawTileTextLine(
        const std::wstring& text,
        RenderTextFormat format,
        const RenderRect& bounds,
        ID2D1SolidColorBrush* text_brush,
        ID2D1SolidColorBrush* outline_brush,
        DWRITE_TEXT_ALIGNMENT text_alignment)
    {
        DrawTileTextLayout(
            text,
            format,
            bounds,
            text_brush,
            outline_brush,
            text_alignment,
            DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
            DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    float Direct2DRenderer::MeasureTextWidth(const std::wstring& text, RenderTextFormat format) const
    {
        if (text.empty() || !GetFormat(format) || !m_dwrite_factory)
        {
            return 0.0f;
        }

        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = m_dwrite_factory->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            GetFormat(format),
            4096.0f,
            64.0f,
            layout.GetAddressOf());
        if (FAILED(hr) || !layout)
        {
            return 0.0f;
        }

        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS metrics = {};
        if (FAILED(layout->GetMetrics(&metrics)))
        {
            return 0.0f;
        }

        return metrics.widthIncludingTrailingWhitespace;
    }

    RenderTextMetrics Direct2DRenderer::MeasureTextLayout(
        const std::wstring& text,
        RenderTextFormat format,
        float max_width,
        float max_height,
        DWRITE_WORD_WRAPPING word_wrapping) const
    {
        ComPtr<IDWriteTextLayout> layout = CreateTextLayout(
            text,
            format,
            MakeRenderRect(0.0f, 0.0f, max_width, max_height),
            word_wrapping);
        if (!layout)
        {
            return {};
        }

        DWRITE_TEXT_METRICS metrics = {};
        if (FAILED(layout->GetMetrics(&metrics)))
        {
            return {};
        }

        return RenderTextMetrics{
            metrics.widthIncludingTrailingWhitespace,
            metrics.height
        };
    }

    void Direct2DRenderer::DrawTextLayoutAt(
        const std::wstring& text,
        RenderTextFormat format,
        float max_width,
        float max_height,
        DWRITE_WORD_WRAPPING word_wrapping,
        const RenderPoint& origin,
        ID2D1SolidColorBrush* brush)
    {
        ID2D1SolidColorBrush* text_brush = brush;
        if (!m_render_target || !text_brush)
        {
            return;
        }

        ComPtr<IDWriteTextLayout> layout = CreateTextLayout(
            text,
            format,
            MakeRenderRect(0.0f, 0.0f, max_width, max_height),
            word_wrapping);
        if (!layout)
        {
            return;
        }

        m_render_target->DrawTextLayout(
            ToD2DPoint(origin),
            layout.Get(),
            text_brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    HRESULT Direct2DRenderer::CreateBrush(ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& color)
    {
        return m_render_target->CreateSolidColorBrush(color, brush.GetAddressOf());
    }

    HRESULT Direct2DRenderer::CreateTextFormats()
    {
        HRESULT hr = m_dwrite_factory->CreateTextFormat(
            L"Segoe UI Variable Display",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            Scale(28.0f),
            L"",
            m_title_format.GetAddressOf());
        if (FAILED(hr))
        {
            hr = m_dwrite_factory->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                Scale(28.0f),
                L"",
                m_title_format.GetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }
        }

        hr = m_dwrite_factory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            Scale(14.0f),
            L"",
            m_body_format.GetAddressOf());
        if (FAILED(hr))
        {
            return hr;
        }

        hr = m_dwrite_factory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            Scale(12.0f),
            L"",
            m_small_format.GetAddressOf());
        if (FAILED(hr))
        {
            return hr;
        }

        m_body_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_small_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        return m_dwrite_factory->CreateEllipsisTrimmingSign(
            m_small_format.Get(),
            m_ellipsis_trimming_sign.GetAddressOf());
    }

    void Direct2DRenderer::ApplyTrimming(IDWriteTextLayout* layout, DWRITE_WORD_WRAPPING word_wrapping) const
    {
        if (!layout || word_wrapping != DWRITE_WORD_WRAPPING_NO_WRAP)
        {
            return;
        }

        DWRITE_TRIMMING trimming = {};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        layout->SetTrimming(&trimming, m_ellipsis_trimming_sign.Get());
    }

    ComPtr<IDWriteTextLayout> Direct2DRenderer::CreateTextLayout(
        const std::wstring& text,
        RenderTextFormat format,
        const RenderRect& bounds,
        DWRITE_WORD_WRAPPING word_wrapping) const
    {
        ComPtr<IDWriteTextLayout> layout;
        IDWriteTextFormat* text_format = GetFormat(format);
        if (!m_dwrite_factory || !text_format)
        {
            return layout;
        }

        const float width = std::max(bounds.right - bounds.left, 1.0f);
        const float height = std::max(bounds.bottom - bounds.top, 1.0f);
        HRESULT hr = m_dwrite_factory->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            text_format,
            width,
            height,
            layout.GetAddressOf());
        if (FAILED(hr) || !layout)
        {
            layout.Reset();
            return layout;
        }

        layout->SetWordWrapping(word_wrapping);
        ApplyTrimming(layout.Get(), word_wrapping);
        return layout;
    }

    IDWriteTextFormat* Direct2DRenderer::GetFormat(RenderTextFormat format) const
    {
        switch (format)
        {
        case RenderTextFormat::Title:
            return m_title_format.Get();
        case RenderTextFormat::Body:
            return m_body_format.Get();
        case RenderTextFormat::Small:
        default:
            return m_small_format.Get();
        }
    }

    float Direct2DRenderer::Scale(float value) const
    {
        return value * m_ui_scale;
    }

    MainWindowPainter::MainWindowPainter()
        : m_renderer(std::make_unique<Direct2DRenderer>())
    {
    }

    MainWindowPainter::~MainWindowPainter() = default;

    HRESULT MainWindowPainter::CreateDeviceResources(HWND hwnd, const RenderVisualPalette& palette)
    {
        return m_renderer->CreateDeviceResources(hwnd, palette);
    }

    void MainWindowPainter::DiscardDeviceResources()
    {
        m_renderer->DiscardDeviceResources();
    }

    bool MainWindowPainter::HasRenderTarget() const
    {
        return m_renderer->HasRenderTarget();
    }

    HRESULT MainWindowPainter::Resize(HWND hwnd)
    {
        return m_renderer->Resize(hwnd);
    }

    void MainWindowPainter::Paint(HWND hwnd, const MainWindowDrawingState& state)
    {
        m_renderer->Paint(hwnd, state);
    }

    RenderRect MainWindowPainter::GetHoverTooltipBounds(HWND hwnd, const MainWindowDrawingState& state)
    {
        return m_renderer->GetHoverTooltipBounds(hwnd, state);
    }

    void Direct2DRenderer::Paint(HWND hwnd, const MainWindowDrawingState& state)
    {
        HRESULT hr = CreateDeviceResources(hwnd, state.palette);
        if (FAILED(hr))
        {
            return;
        }

        BeginDraw();
        SetIdentityTransform();
        const bool has_clip = !IsRenderRectEmpty(state.paint_rect);
        if (has_clip)
        {
            PushClip(state.paint_rect);
            FillRectangle(state.paint_rect, m_window_background_brush.Get());
        }
        else
        {
            Clear(MakeD2DColor(
                state.palette.window_background_red,
                state.palette.window_background_green,
                state.palette.window_background_blue));
        }

        DrawHeader(state);
        DrawTreemap(state);
        DrawStatusBar(state);
        DrawHoverTooltip(state);

        if (has_clip)
        {
            PopClip();
        }

        if (EndDrawNeedsRecreate())
        {
            DiscardDeviceResources();
        }
    }

    RenderRect Direct2DRenderer::GetHoverTooltipBounds(HWND hwnd, const MainWindowDrawingState& state)
    {
        HRESULT hr = CreateDeviceResources(hwnd, state.palette);
        if (FAILED(hr))
        {
            return MakeRenderRect(0, 0, 0, 0);
        }

        HoverTooltipLayout layout;
        if (!BuildHoverTooltipLayout(state, &layout))
        {
            return MakeRenderRect(0, 0, 0, 0);
        }

        const RenderRect bounds = MakeRenderRect(
            std::min(layout.shadow_rect.left, layout.tooltip_rect.left),
            std::min(layout.shadow_rect.top, layout.tooltip_rect.top),
            std::max(layout.shadow_rect.right, layout.tooltip_rect.right),
            std::max(layout.shadow_rect.bottom, layout.tooltip_rect.bottom));
        return InflateRenderRect(bounds, Scale(3.0f));
    }

    std::wstring Direct2DRenderer::BuildDirectoryHeaderText(
        const std::wstring& label,
        uint64_t size,
        float available_width) const
    {
        const std::wstring size_suffix = L" - " + HumanizeBytes(size);
        const std::wstring full_text = label + size_suffix;
        if (MeasureTextWidth(full_text, RenderTextFormat::Small) <= available_width)
        {
            return full_text;
        }

        constexpr size_t kMinimumReservedNameCharacters = 5;
        const float suffix_width = MeasureTextWidth(size_suffix, RenderTextFormat::Small);
        if (suffix_width >= available_width)
        {
            return label;
        }

        size_t low = 0;
        size_t high = label.size();
        size_t best = 0;
        while (low <= high)
        {
            const size_t mid = low + ((high - low) / 2);
            const std::wstring candidate = label.substr(0, mid) + L"..." + size_suffix;
            if (MeasureTextWidth(candidate, RenderTextFormat::Small) <= available_width)
            {
                best = mid;
                low = mid + 1;
            }
            else
            {
                if (mid == 0)
                {
                    break;
                }
                high = mid - 1;
            }
        }

        if (best < kMinimumReservedNameCharacters)
        {
            return label;
        }

        return label.substr(0, best) + L"..." + size_suffix;
    }

void Direct2DRenderer::DrawPanelRect(
    const RenderRect& rect,
    ID2D1SolidColorBrush* fill_brush,
    ID2D1SolidColorBrush* border_brush,
    float stroke_width)
{
    FillRectangle(rect, fill_brush);
    DrawRectangle(rect, border_brush, stroke_width);
}

void Direct2DRenderer::DrawHeaderGroupBox(const RenderRect& rect, const std::wstring& label)
{
    if (IsRenderRectEmpty(rect) || label.empty())
    {
        return;
    }

    const float label_padding_x = Scale(5.0f);
    const float label_left = rect.left + Scale(10.0f);
    const float measured_label_width = std::ceil(MeasureTextWidth(label, RenderTextFormat::Small));
    const float label_right = std::min(
        rect.right - Scale(8.0f),
        label_left + measured_label_width + (label_padding_x * 2.0f));
    const float label_baseline_gap = Scale(4.0f);
    const float top_y = std::floor(rect.top) + 0.5f;
    const float bottom_y = std::floor(rect.bottom) + 0.5f;
    const float left_x = std::floor(rect.left) + 0.5f;
    const float right_x = std::floor(rect.right) + 0.5f;

    DrawLine(
        MakeRenderPoint(left_x, top_y),
        MakeRenderPoint(std::max(left_x, label_left - label_baseline_gap), top_y),
        m_border_brush.Get(),
        Scale(1.0f));
    DrawLine(
        MakeRenderPoint(std::min(right_x, label_right + label_baseline_gap), top_y),
        MakeRenderPoint(right_x, top_y),
        m_border_brush.Get(),
        Scale(1.0f));
    DrawLine(
        MakeRenderPoint(left_x, top_y),
        MakeRenderPoint(left_x, bottom_y),
        m_border_brush.Get(),
        Scale(1.0f));
    DrawLine(
        MakeRenderPoint(right_x, top_y),
        MakeRenderPoint(right_x, bottom_y),
        m_border_brush.Get(),
        Scale(1.0f));
    DrawLine(
        MakeRenderPoint(left_x, bottom_y),
        MakeRenderPoint(right_x, bottom_y),
        m_border_brush.Get(),
        Scale(1.0f));

    DrawTextLine(
        label,
        RenderTextFormat::Small,
        MakeRenderRect(
            label_left + label_padding_x,
            rect.top - Scale(8.0f),
            label_right,
            rect.top + Scale(8.0f)),
        m_muted_brush.Get());
}

void Direct2DRenderer::DrawTreemap(const MainWindowDrawingState& state)
{
    if (!IsRenderRectEmpty(state.paint_rect) && !RenderRectsIntersect(state.canvas_rect, state.paint_rect))
    {
        return;
    }

    const float treemap_bottom_slice_height = Scale(kTreemapBottomSliceHeight);
    const float content_invalidation_padding = Scale(3.0f);
    const float hover_stroke_width = Scale(2.0f);
    const float regular_stroke_width = Scale(0.75f);

    DrawPanelRect(state.canvas_rect, m_canvas_brush.Get(), m_border_brush.Get(), 1.2f);

    if (state.scan_in_progress)
    {
        DrawTextLine(
            L"Scanning...",
            RenderTextFormat::Title,
            MakeRenderRect(
                state.canvas_rect.left + Scale(24.0f),
                state.canvas_rect.top + Scale(28.0f),
                state.canvas_rect.right - Scale(24.0f),
                state.canvas_rect.top + Scale(70.0f)),
            m_text_brush.Get());
        DrawTextLine(
            state.status_text,
            RenderTextFormat::Body,
            MakeRenderRect(
                state.canvas_rect.left + Scale(24.0f),
                state.canvas_rect.top + Scale(76.0f),
                state.canvas_rect.right - Scale(24.0f),
                state.canvas_rect.top + Scale(100.0f)),
            m_muted_brush.Get());
        return;
    }

    if (state.has_error)
    {
        DrawTextLine(
            L"Scan failed",
            RenderTextFormat::Title,
            MakeRenderRect(
                state.canvas_rect.left + Scale(24.0f),
                state.canvas_rect.top + Scale(28.0f),
                state.canvas_rect.right - Scale(24.0f),
                state.canvas_rect.top + Scale(70.0f)),
            m_error_brush.Get());
        DrawTextLine(
            state.error_text,
            RenderTextFormat::Body,
            MakeRenderRect(
                state.canvas_rect.left + Scale(24.0f),
                state.canvas_rect.top + Scale(76.0f),
                state.canvas_rect.right - Scale(24.0f),
                state.canvas_rect.bottom - Scale(24.0f)),
            m_muted_brush.Get());
        return;
    }

    if (!state.tree)
    {
        DrawTextLine(
            L"No scan data yet",
            RenderTextFormat::Title,
            MakeRenderRect(
                state.canvas_rect.left + Scale(24.0f),
                state.canvas_rect.top + Scale(28.0f),
                state.canvas_rect.right - Scale(24.0f),
                state.canvas_rect.top + Scale(70.0f)),
            m_text_brush.Get());
        DrawTextLine(
            L"Choose a folder above, then click Scan or Browse to start.",
            RenderTextFormat::Body,
            MakeRenderRect(
                state.canvas_rect.left + Scale(24.0f),
                state.canvas_rect.top + Scale(78.0f),
                state.canvas_rect.right - Scale(24.0f),
                state.canvas_rect.top + Scale(104.0f)),
            m_muted_brush.Get());
        return;
    }

    if (!state.rects || !state.breadcrumbs)
    {
        return;
    }

    const auto& rects = *state.rects;
    const auto& breadcrumbs = *state.breadcrumbs;
    const Node* focused_node = state.tree->GetNode(state.view_root_id);
    const std::wstring focused_path = focused_node
        ? state.tree->BuildPath(state.view_root_id)
        : (state.selected_path.empty() ? L"(no path selected)" : state.selected_path);
    const std::wstring focused_name = focused_node
        ? (focused_node->name.empty() ? focused_path : focused_node->name)
        : (state.selected_path.empty() ? L"(no path selected)" : state.selected_path);
    const std::wstring focused_size = focused_node ? HumanizeBytes(focused_node->size) : L"-";

    const visualization::TreemapVisualStyle frame_style = visualization::BuildTreemapVisualStyle(
        focused_name,
        focused_path,
        true);
    if (breadcrumbs.empty())
    {
        const RenderRect shadow_rect = MakeRenderRect(
            state.focus_up_rect.left + Scale(8.0f),
            state.focus_up_rect.top + Scale(8.0f),
            state.focus_up_rect.right + Scale(8.0f),
            state.focus_up_rect.bottom + Scale(8.0f));
        FillRectangle(shadow_rect, m_active_level_shadow_brush.Get());
        const RenderRect glow_rect = MakeRenderRect(
            state.focus_up_rect.left - Scale(2.0f),
            state.focus_up_rect.top - Scale(2.0f),
            state.focus_up_rect.right + Scale(2.0f),
            state.focus_up_rect.bottom + Scale(2.0f));
        DrawRectangle(glow_rect, m_active_level_glow_brush.Get(), Scale(5.5f));
        m_header_action_brush->SetColor(ParseHexColor(frame_style.fill_color));
        FillRectangle(state.focus_up_rect, m_header_action_brush.Get());
        DrawRectangle(state.focus_up_rect, m_hover_brush.Get(), Scale(2.8f));
        m_tile_text_brush->SetColor(ParseHexColor(frame_style.label_text_color));
        m_tile_text_outline_brush->SetColor(ParseHexColor(frame_style.text_outline_color, 0.78f));
        std::wstring frame_text = focused_name + L" - " + focused_size;
        DrawTileTextLine(
            frame_text,
            RenderTextFormat::Small,
            MakeRenderRect(
                state.focus_up_rect.left + Scale(8.0f),
                state.focus_up_rect.top + Scale(3.0f),
                state.focus_up_rect.right - Scale(8.0f),
                state.focus_up_rect.bottom - Scale(3.0f)),
            m_tile_text_brush.Get(),
            m_tile_text_outline_brush.Get());
    }
    else
    {
        for (size_t index = 0; index < breadcrumbs.size(); ++index)
        {
            const BreadcrumbStrip& strip = breadcrumbs[index];
            if (!IsRenderRectEmpty(state.paint_rect) &&
                !RenderRectsIntersect(InflateRenderRect(strip.frame_rect, Scale(12.0f)), state.paint_rect))
            {
                continue;
            }

            const bool is_hovered = static_cast<int>(index) == state.hovered_breadcrumb_index;
            const bool is_active_level = strip.node_id == state.view_root_id;

            const RenderRect left_slice_rect = MakeRenderRect(
                strip.frame_rect.left,
                strip.rect.bottom,
                strip.content_rect.left,
                strip.frame_rect.bottom - treemap_bottom_slice_height);
            const RenderRect right_slice_rect = MakeRenderRect(
                strip.content_rect.right,
                strip.rect.bottom,
                strip.frame_rect.right,
                strip.frame_rect.bottom - treemap_bottom_slice_height);
            const RenderRect bottom_slice_rect = MakeRenderRect(
                strip.frame_rect.left,
                strip.frame_rect.bottom - treemap_bottom_slice_height,
                strip.frame_rect.right,
                strip.frame_rect.bottom);

            if (is_active_level)
            {
                const RenderRect shadow_rect = MakeRenderRect(
                    strip.frame_rect.left + Scale(8.0f),
                    strip.frame_rect.top + Scale(8.0f),
                    strip.frame_rect.right + Scale(8.0f),
                    strip.frame_rect.bottom + Scale(8.0f));
                FillRectangle(shadow_rect, m_active_level_shadow_brush.Get());
                const RenderRect glow_rect = MakeRenderRect(
                    strip.frame_rect.left - Scale(2.0f),
                    strip.frame_rect.top - Scale(2.0f),
                    strip.frame_rect.right + Scale(2.0f),
                    strip.frame_rect.bottom + Scale(2.0f));
                DrawRectangle(glow_rect, m_active_level_glow_brush.Get(), Scale(5.5f));
            }

            m_header_action_brush->SetColor(ParseHexColor(strip.style.fill_color, 0.88f));
            FillRectangle(left_slice_rect, m_header_action_brush.Get());
            FillRectangle(right_slice_rect, m_header_action_brush.Get());
            FillRectangle(bottom_slice_rect, m_header_action_brush.Get());

            m_header_action_brush->SetColor(ParseHexColor(strip.style.fill_color));
            FillRectangle(strip.rect, m_header_action_brush.Get());
            m_tile_text_plate_brush->SetColor(ParseHexColor(strip.style.text_plate_color, 0.42f));
            const RenderRect strip_plate_rect = MakeRenderRect(
                strip.rect.left + Scale(1.0f),
                strip.rect.top + Scale(1.0f),
                std::max(strip.rect.left + Scale(2.0f), strip.rect.right - Scale(1.0f)),
                std::max(strip.rect.top + Scale(2.0f), strip.rect.bottom - Scale(1.0f)));
            FillRoundedRectangle(strip_plate_rect, Scale(2.0f), Scale(2.0f), m_tile_text_plate_brush.Get());
            DrawRectangle(
                strip.rect,
                is_hovered ? m_hover_brush.Get() : m_border_brush.Get(),
                is_hovered ? Scale(1.6f) : Scale(0.9f));
            if (is_active_level)
            {
                DrawRectangle(strip.frame_rect, m_hover_brush.Get(), Scale(2.8f));
            }

            m_tile_text_brush->SetColor(ParseHexColor(strip.style.label_text_color));
            m_tile_text_outline_brush->SetColor(ParseHexColor(strip.style.text_outline_color, 0.78f));
            DrawTileTextLine(
                strip.text,
                RenderTextFormat::Small,
                MakeRenderRect(
                    strip.rect.left + Scale(8.0f),
                    strip.rect.top + Scale(3.0f),
                    strip.rect.right - Scale(8.0f),
                    strip.rect.bottom - Scale(3.0f)),
                m_tile_text_brush.Get(),
                m_tile_text_outline_brush.Get());
        }
    }

    if (!IsRenderRectEmpty(state.paint_rect) && !RenderRectsIntersect(state.treemap_content_rect, state.paint_rect))
    {
        return;
    }

    m_tile_brush->SetColor(ParseHexColor(frame_style.fill_color));
    FillRectangle(state.treemap_content_rect, m_tile_brush.Get());

    for (size_t index = 0; index < rects.size(); ++index)
    {
        const visualization::TreemapRect& rect = rects[index];
        const RenderRect draw_rect = MakeRenderRect(
            state.treemap_content_rect.left + static_cast<float>(rect.x),
            state.treemap_content_rect.top + static_cast<float>(rect.y),
            state.treemap_content_rect.left + static_cast<float>(rect.x + rect.width),
            state.treemap_content_rect.top + static_cast<float>(rect.y + rect.height));
        if (!IsRenderRectEmpty(state.paint_rect) &&
            !RenderRectsIntersect(InflateRenderRect(draw_rect, content_invalidation_padding), state.paint_rect))
        {
            continue;
        }

        m_tile_brush->SetColor(ParseHexColor(rect.fill_color));
        FillRectangle(draw_rect, m_tile_brush.Get());

        const bool is_hovered = static_cast<int>(index) == state.hovered_index;
        ID2D1SolidColorBrush* border_brush = is_hovered ? m_hover_brush.Get() : m_border_brush.Get();
        const float stroke_width = is_hovered ? hover_stroke_width : regular_stroke_width;
        const float tile_width = draw_rect.right - draw_rect.left;
        const float tile_height = draw_rect.bottom - draw_rect.top;
        const bool is_view_root = rect.node_id == state.view_root_id;
        const bool is_directory_header = rect.is_dir && !is_view_root;

        if (is_directory_header && tile_width >= Scale(4.0f) && tile_height >= Scale(4.0f))
        {
            const float header_height = std::min(
                std::max(Scale(3.0f), static_cast<float>(state.layout_options.directory_header_height) - Scale(2.0f)),
                std::max(0.0f, tile_height - Scale(2.0f)));
            if (header_height > 0.0f)
            {
                m_tile_text_plate_brush->SetColor(ParseHexColor(rect.text_plate_color, 0.42f));
                const RenderRect header_plate_rect = MakeRenderRect(
                    draw_rect.left + Scale(1.0f),
                    draw_rect.top + Scale(1.0f),
                    std::max(draw_rect.left + Scale(2.0f), draw_rect.right - Scale(1.0f)),
                    std::min(draw_rect.bottom - Scale(1.0f), draw_rect.top + Scale(1.0f) + header_height));
                FillRoundedRectangle(header_plate_rect, Scale(2.0f), Scale(2.0f), m_tile_text_plate_brush.Get());
            }
        }

        DrawRectangle(draw_rect, border_brush, stroke_width);

        if (tile_width >= Scale(56.0f) && tile_height >= Scale(18.0f))
        {
            m_tile_text_brush->SetColor(ParseHexColor(rect.label_text_color));
            m_tile_secondary_text_brush->SetColor(ParseHexColor(rect.secondary_text_color));
            m_tile_text_outline_brush->SetColor(ParseHexColor(rect.text_outline_color, 0.78f));
            m_tile_text_plate_brush->SetColor(ParseHexColor(rect.text_plate_color, 0.34f));

            const bool is_leaf_file = !rect.is_dir;
            const bool show_label = tile_width >= Scale(92.0f) && tile_height >= Scale(34.0f) && !is_directory_header;
            if (!is_leaf_file && !is_view_root && !is_directory_header)
            {
                const float plate_height = show_label ? Scale(30.0f) : Scale(16.0f);
                const RenderRect plate_rect = MakeRenderRect(
                    draw_rect.left + Scale(2.0f),
                    draw_rect.top + Scale(2.0f),
                    std::max(draw_rect.left + Scale(4.0f), draw_rect.right - Scale(2.0f)),
                    std::min(draw_rect.bottom - Scale(2.0f), draw_rect.top + Scale(2.0f) + plate_height));
                FillRoundedRectangle(plate_rect, Scale(2.0f), Scale(2.0f), m_tile_text_plate_brush.Get());
            }

            if (is_directory_header)
            {
                const RenderRect header_rect = MakeRenderRect(
                    draw_rect.left + Scale(6.0f),
                    draw_rect.top + Scale(2.0f),
                    draw_rect.right - Scale(6.0f),
                    draw_rect.top + Scale(16.0f));
                const float header_width = std::max(0.0f, header_rect.right - header_rect.left);
                const std::wstring header_text = BuildDirectoryHeaderText(
                    rect.label,
                    rect.size,
                    header_width);

                DrawTileTextLine(
                    header_text,
                    RenderTextFormat::Small,
                    header_rect,
                    m_tile_text_brush.Get(),
                    m_tile_text_outline_brush.Get(),
                    DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            else if (!is_view_root && tile_width >= Scale(64.0f) && tile_height >= Scale(40.0f))
            {
                const std::wstring size_text = HumanizeBytes(rect.size);
                const float margin = Scale(6.0f);
                const float size_height = Scale(14.0f);
                const float gap = Scale(4.0f);
                const float block_height = std::min(tile_height - Scale(12.0f), Scale(42.0f));
                const float block_top = draw_rect.top + std::max(Scale(4.0f), (tile_height - block_height) * 0.42f);
                const bool can_stack_label_and_size =
                    tile_height >= Scale(48.0f) &&
                    MeasureTextWidth(size_text, RenderTextFormat::Small) <= (tile_width - (margin * 2.0f));
                if (!can_stack_label_and_size)
                {
                    const RenderRect size_rect = MakeRenderRect(
                        draw_rect.left + margin,
                        draw_rect.top + Scale(3.0f),
                        draw_rect.right - margin,
                        draw_rect.bottom - Scale(3.0f));
                    DrawTileTextLayout(
                        size_text,
                        RenderTextFormat::Small,
                        size_rect,
                        m_tile_secondary_text_brush.Get(),
                        m_tile_text_outline_brush.Get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                        DWRITE_WORD_WRAPPING_NO_WRAP);
                    continue;
                }

                const RenderRect label_rect = MakeRenderRect(
                    draw_rect.left + margin,
                    block_top,
                    draw_rect.right - margin,
                    block_top + block_height - size_height - gap);
                const RenderRect size_rect = MakeRenderRect(
                    draw_rect.left + margin,
                    block_top + block_height - size_height,
                    draw_rect.right - margin,
                    block_top + block_height);

                DrawTileTextLayout(
                    rect.label,
                    RenderTextFormat::Small,
                    label_rect,
                    m_tile_text_brush.Get(),
                    m_tile_text_outline_brush.Get(),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                    DWRITE_WORD_WRAPPING_NO_WRAP);
                DrawTileTextLayout(
                    size_text,
                    RenderTextFormat::Small,
                    size_rect,
                    m_tile_secondary_text_brush.Get(),
                    m_tile_text_outline_brush.Get(),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                    DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            else
            {
                const std::wstring size_text = HumanizeBytes(rect.size);
                const bool can_show_label = tile_width >= Scale(44.0f) && tile_height >= Scale(24.0f);
                const bool can_show_size = tile_width >= Scale(32.0f) && tile_height >= Scale(14.0f);
                const bool can_stack_label_and_size = can_show_label && can_show_size && tile_height >= Scale(28.0f);
                if (can_stack_label_and_size)
                {
                    const float block_height = std::min(tile_height - Scale(5.0f), Scale(24.0f));
                    const float block_top = draw_rect.top + std::max(Scale(1.0f), (tile_height - block_height) * 0.5f);
                    const RenderRect label_rect = MakeRenderRect(
                        draw_rect.left + Scale(4.0f),
                        block_top,
                        draw_rect.right - Scale(4.0f),
                        block_top + Scale(9.0f));
                    const RenderRect size_rect = MakeRenderRect(
                        draw_rect.left + Scale(4.0f),
                        block_top + Scale(11.0f),
                        draw_rect.right - Scale(4.0f),
                        block_top + block_height);
                    DrawTileTextLayout(
                        rect.label,
                        RenderTextFormat::Small,
                        label_rect,
                        m_tile_text_brush.Get(),
                        m_tile_text_outline_brush.Get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                        DWRITE_WORD_WRAPPING_NO_WRAP);
                    DrawTileTextLayout(
                        size_text,
                        RenderTextFormat::Small,
                        size_rect,
                        m_tile_secondary_text_brush.Get(),
                        m_tile_text_outline_brush.Get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                        DWRITE_WORD_WRAPPING_NO_WRAP);
                }
                else if (can_show_size)
                {
                    const RenderRect size_rect = MakeRenderRect(
                        draw_rect.left + Scale(3.0f),
                        draw_rect.top + Scale(1.0f),
                        draw_rect.right - Scale(3.0f),
                        draw_rect.bottom - Scale(1.0f));
                    DrawTileTextLayout(
                        size_text,
                        RenderTextFormat::Small,
                        size_rect,
                        m_tile_secondary_text_brush.Get(),
                        m_tile_text_outline_brush.Get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                        DWRITE_WORD_WRAPPING_NO_WRAP);
                }
            }
        }
    }
}

void Direct2DRenderer::DrawHeader(const MainWindowDrawingState& state)
{
    const RenderRect header_rect = MakeRenderRect(
        Scale(kOuterPadding),
        Scale(kOuterPadding - 8.0f),
        state.canvas_rect.right,
        Scale(kOuterPadding + kHeaderHeight - 8.0f));
    if (!IsRenderRectEmpty(state.paint_rect) && !RenderRectsIntersect(header_rect, state.paint_rect))
    {
        return;
    }

    DrawPanelRect(header_rect, m_panel_brush.Get(), m_border_brush.Get(), Scale(1.2f));

    const float header_separator_left_offset = Scale(8.0f);
    const float separator_x = std::floor(
        ((state.header_info_rect.right + state.scan_group_rect.left) * 0.5f) - header_separator_left_offset);
    if (separator_x > state.header_info_rect.left && separator_x < header_rect.right)
    {
        const float separator_top = std::min(header_rect.bottom - Scale(8.0f), state.scan_group_rect.top);
        const float separator_bottom = std::min(
            header_rect.bottom - Scale(8.0f),
            std::max(state.display_group_rect.bottom, state.treemap_group_rect.bottom) + Scale(1.0f));
        DrawLine(
            MakeRenderPoint(separator_x + Scale(1.0f), separator_top),
            MakeRenderPoint(separator_x + Scale(1.0f), separator_bottom),
            m_separator_brush.Get(),
            Scale(2.0f));
    }

    DrawHeaderGroupBox(state.display_group_rect, L"Show");
    DrawHeaderGroupBox(state.treemap_group_rect, L"Treemap");

    DrawTextLine(
        L"SwiftDisk",
        RenderTextFormat::Title,
        MakeRenderRect(
            state.header_info_rect.left,
            state.header_info_rect.top,
            state.header_info_rect.right,
            state.header_info_rect.top + Scale(34.0f)),
        m_text_brush.Get());

    const Node* focused_node = state.tree && state.view_root_id != 0
        ? state.tree->GetNode(state.view_root_id)
        : nullptr;
    const std::wstring focused_path = focused_node
        ? state.tree->BuildPath(state.view_root_id)
        : (state.selected_path.empty() ? L"(no path selected)" : state.selected_path);
    const std::wstring focused_size = focused_node
        ? HumanizeBytes(focused_node->size)
        : L"-";

    DrawTextLine(
        focused_path,
        RenderTextFormat::Small,
        MakeRenderRect(
            state.header_info_rect.left,
            state.header_info_rect.top + Scale(42.0f),
            state.header_info_rect.right,
            state.header_info_rect.top + Scale(58.0f)),
        m_muted_brush.Get());

    std::wstringstream summary;
    if (state.scan_in_progress)
    {
        summary << L"Backend: " << (state.requested_path_is_ntfs ? L"NTFS MFT" : L"Directory")
            << L"  |  Status: scanning";
    }
    else if (state.tree)
    {
        summary << L"Backend: " << (state.used_mft_scan ? L"NTFS MFT" : L"Directory")
            << L"  |  Focus size: " << focused_size
            << L"  |  Nodes: " << state.tree->NodeCount()
            << L"  |  Scan: " << FormatSeconds(state.elapsed_seconds);
    }
    else if (state.has_error)
    {
        summary << L"Status: failed";
    }
    else
    {
        summary << L"Status: idle";
    }

    DrawTextLine(
        summary.str(),
        RenderTextFormat::Body,
        MakeRenderRect(
            state.header_info_rect.left,
            state.header_info_rect.top + Scale(68.0f),
            state.header_info_rect.right,
            state.header_info_rect.top + Scale(86.0f)),
        m_muted_brush.Get());
}

RenderRect Direct2DRenderer::GetHoveredRectBounds(const MainWindowDrawingState& state, const visualization::TreemapRect& rect) const
{
    return MakeRenderRect(
        state.treemap_content_rect.left + static_cast<float>(rect.x),
        state.treemap_content_rect.top + static_cast<float>(rect.y),
        state.treemap_content_rect.left + static_cast<float>(rect.x + rect.width),
        state.treemap_content_rect.top + static_cast<float>(rect.y + rect.height));
}

bool Direct2DRenderer::TryGetHoverDisplayInfo(const MainWindowDrawingState& state, 
    std::wstring* name_text,
    std::wstring* size_text,
    RenderRect* hovered_bounds,
    bool* follow_tooltip) const
{
    if (!name_text || !size_text || !hovered_bounds || !follow_tooltip)
    {
        return false;
    }

    if (state.scan_in_progress || !state.tree || !state.rects || !state.breadcrumbs)
    {
        return false;
    }

    const auto& rects = *state.rects;
    const auto& breadcrumbs = *state.breadcrumbs;
    if (state.hovered_breadcrumb_index >= 0 && state.hovered_breadcrumb_index < static_cast<int>(breadcrumbs.size()))
    {
        const BreadcrumbStrip& strip = breadcrumbs[static_cast<size_t>(state.hovered_breadcrumb_index)];
        *hovered_bounds = strip.rect;
        *follow_tooltip = true;

        if (state.tree)
        {
            const Node* node = state.tree->GetNode(strip.node_id);
            const std::wstring path = state.tree->BuildPath(strip.node_id);
            *name_text = path.empty() ? strip.text : path;
            *size_text = L"Size: " + (node ? HumanizeBytes(node->size) : L"-");
        }
        else
        {
            *name_text = strip.text;
            *size_text = L"Size: -";
        }
        return true;
    }

    if (state.hovered_index >= 0 && state.hovered_index < static_cast<int>(rects.size()))
    {
        const visualization::TreemapRect& rect = rects[static_cast<size_t>(state.hovered_index)];
        *hovered_bounds = GetHoveredRectBounds(state, rect);
        *follow_tooltip = true;
        if (state.tree)
        {
            const std::wstring path = state.tree->BuildPath(rect.node_id);
            *name_text = path.empty() ? rect.label : path;
        }
        else
        {
            *name_text = rect.label;
        }
        *size_text = L"Size: " + HumanizeBytes(rect.size);
        return true;
    }

    return false;
}

RenderPoint Direct2DRenderer::ResolveTooltipPosition(const MainWindowDrawingState& state,
    const RenderPoint& anchor,
    const RenderRect& hovered_rect,
    float tooltip_width,
    float tooltip_height,
    bool follow_tooltip) const
{
    const float tooltip_inner_margin = Scale(kTooltipInnerMargin);
    float tooltip_left = anchor.x + Scale(kTooltipOffsetX);
    float tooltip_top = anchor.y + Scale(kTooltipOffsetY);

    if (tooltip_left + tooltip_width > state.canvas_rect.right - tooltip_inner_margin)
    {
        tooltip_left = anchor.x - tooltip_width - Scale(10.0f);
    }
    if (tooltip_top + tooltip_height > state.canvas_rect.bottom - tooltip_inner_margin)
    {
        tooltip_top = anchor.y - tooltip_height - Scale(10.0f);
    }

    if (!follow_tooltip)
    {
        const float hovered_width = hovered_rect.right - hovered_rect.left;
        const float hovered_height = hovered_rect.bottom - hovered_rect.top;
        if (hovered_width >= tooltip_width + (tooltip_inner_margin * 2.0f) &&
            hovered_height >= tooltip_height + (tooltip_inner_margin * 2.0f))
        {
            const float min_hovered_left = hovered_rect.left + tooltip_inner_margin;
            const float max_hovered_left = hovered_rect.right - tooltip_width - tooltip_inner_margin;
            const float min_hovered_top = hovered_rect.top + tooltip_inner_margin;
            const float max_hovered_top = hovered_rect.bottom - tooltip_height - tooltip_inner_margin;
            tooltip_left = std::clamp(tooltip_left, min_hovered_left, max_hovered_left);
            tooltip_top = std::clamp(tooltip_top, min_hovered_top, max_hovered_top);
        }
    }

    const float min_tooltip_left = state.canvas_rect.left + tooltip_inner_margin;
    const float max_tooltip_left = std::max(min_tooltip_left, state.canvas_rect.right - tooltip_width - tooltip_inner_margin);
    const float min_tooltip_top = state.canvas_rect.top + tooltip_inner_margin;
    const float max_tooltip_top = std::max(min_tooltip_top, state.canvas_rect.bottom - tooltip_height - tooltip_inner_margin);
    tooltip_left = std::clamp(tooltip_left, min_tooltip_left, max_tooltip_left);
    tooltip_top = std::clamp(tooltip_top, min_tooltip_top, max_tooltip_top);

    return MakeRenderPoint(tooltip_left, tooltip_top);
}

bool Direct2DRenderer::BuildHoverTooltipLayout(const MainWindowDrawingState& state, HoverTooltipLayout* layout) const
{
    if (!layout)
    {
        return false;
    }

    *layout = HoverTooltipLayout();
    if (!state.show_tooltip)
    {
        return false;
    }

    std::wstring name_text;
    std::wstring size_text;
    RenderRect hovered_bounds = state.canvas_rect;
    bool follow_tooltip = false;
    if (!TryGetHoverDisplayInfo(state, &name_text, &size_text, &hovered_bounds, &follow_tooltip))
    {
        return false;
    }

    const float tooltip_padding_x = Scale(kTooltipPaddingX);
    const float tooltip_padding_y = Scale(kTooltipPaddingY);
    const float tooltip_line_gap = Scale(kTooltipLineGap);
    const float available_tooltip_width =
        std::max(Scale(48.0f), (state.canvas_rect.right - state.canvas_rect.left) - Scale(12.0f));
    const float max_tooltip_width =
        std::min(Scale(420.0f), std::max(Scale(28.0f), available_tooltip_width - (tooltip_padding_x * 2.0f)));
    const float max_tooltip_height =
        std::max(Scale(48.0f), (state.canvas_rect.bottom - state.canvas_rect.top) - Scale(24.0f));

    ComPtr<IDWriteTextLayout> name_layout = CreateTextLayout(
        name_text,
        RenderTextFormat::Small,
        MakeRenderRect(0.0f, 0.0f, max_tooltip_width, max_tooltip_height),
        DWRITE_WORD_WRAPPING_WRAP);
    ComPtr<IDWriteTextLayout> size_layout = CreateTextLayout(
        size_text,
        RenderTextFormat::Small,
        MakeRenderRect(0.0f, 0.0f, max_tooltip_width, Scale(22.0f)),
        DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!name_layout || !size_layout)
    {
        return false;
    }

    DWRITE_TEXT_METRICS raw_name_metrics = {};
    DWRITE_TEXT_METRICS raw_size_metrics = {};
    if (FAILED(name_layout->GetMetrics(&raw_name_metrics)) ||
        FAILED(size_layout->GetMetrics(&raw_size_metrics)))
    {
        return false;
    }

    const RenderTextMetrics name_metrics = RenderTextMetrics{
        raw_name_metrics.widthIncludingTrailingWhitespace,
        raw_name_metrics.height
    };
    const RenderTextMetrics size_metrics = RenderTextMetrics{
        raw_size_metrics.widthIncludingTrailingWhitespace,
        raw_size_metrics.height
    };
    const float text_width = std::max(
        std::max(
            name_metrics.width_including_trailing_whitespace,
            size_metrics.width_including_trailing_whitespace),
        Scale(96.0f));
    const float name_height = std::max(name_metrics.height, Scale(16.0f));
    const float size_height = std::max(size_metrics.height, Scale(14.0f));
    const float tooltip_width = std::min(available_tooltip_width, text_width + (tooltip_padding_x * 2.0f));
    const float tooltip_height = (tooltip_padding_y * 2.0f) + name_height + tooltip_line_gap + size_height;
    const RenderPoint tooltip_origin = ResolveTooltipPosition(state, 
        state.tooltip_position,
        hovered_bounds,
        tooltip_width,
        tooltip_height,
        follow_tooltip);
    const float tooltip_left = tooltip_origin.x;
    const float tooltip_top = tooltip_origin.y;

    const RenderRect shadow_rect = MakeRenderRect(
        tooltip_left + Scale(2.0f),
        tooltip_top + Scale(3.0f),
        tooltip_left + tooltip_width + Scale(2.0f),
        tooltip_top + tooltip_height + Scale(3.0f));
    const RenderRect tooltip_rect = MakeRenderRect(
        tooltip_left,
        tooltip_top,
        tooltip_left + tooltip_width,
        tooltip_top + tooltip_height);
    const RenderPoint name_origin = MakeRenderPoint(
        tooltip_left + tooltip_padding_x,
        tooltip_top + tooltip_padding_y);
    const RenderPoint size_origin = MakeRenderPoint(
        tooltip_left + tooltip_padding_x,
        tooltip_top + tooltip_padding_y + name_height + tooltip_line_gap);

    layout->name_layout = std::move(name_layout);
    layout->size_layout = std::move(size_layout);
    layout->shadow_rect = shadow_rect;
    layout->tooltip_rect = tooltip_rect;
    layout->name_origin = name_origin;
    layout->size_origin = size_origin;
    return true;
}

void Direct2DRenderer::DrawHoverTooltip(const MainWindowDrawingState& state)
{
    HoverTooltipLayout layout;
    if (!BuildHoverTooltipLayout(state, &layout))
    {
        return;
    }

    const RenderRect tooltip_bounds = MakeRenderRect(
        std::min(layout.shadow_rect.left, layout.tooltip_rect.left),
        std::min(layout.shadow_rect.top, layout.tooltip_rect.top),
        std::max(layout.shadow_rect.right, layout.tooltip_rect.right),
        std::max(layout.shadow_rect.bottom, layout.tooltip_rect.bottom));
    if (!IsRenderRectEmpty(state.paint_rect) &&
        !RenderRectsIntersect(InflateRenderRect(tooltip_bounds, 2.0f), state.paint_rect))
    {
        return;
    }

    FillRoundedRectangle(
        layout.shadow_rect,
        Scale(kTooltipCornerRadius),
        Scale(kTooltipCornerRadius),
        m_tooltip_shadow_brush.Get());
    FillRoundedRectangle(
        layout.tooltip_rect,
        Scale(kTooltipCornerRadius),
        Scale(kTooltipCornerRadius),
        m_tooltip_background_brush.Get());
    DrawRoundedRectangle(
        layout.tooltip_rect,
        Scale(kTooltipCornerRadius),
        Scale(kTooltipCornerRadius),
        m_tooltip_border_brush.Get(),
        Scale(1.0f));

    if (m_render_target && layout.name_layout && layout.size_layout)
    {
        m_render_target->DrawTextLayout(
            ToD2DPoint(layout.name_origin),
            layout.name_layout.Get(),
            m_tooltip_text_brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        m_render_target->DrawTextLayout(
            ToD2DPoint(layout.size_origin),
            layout.size_layout.Get(),
            m_tooltip_secondary_text_brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Direct2DRenderer::DrawStatusBar(const MainWindowDrawingState& state)
{
    if (!state.show_status_bar)
    {
        return;
    }

    if (!IsRenderRectEmpty(state.paint_rect) && !RenderRectsIntersect(state.status_bar_rect, state.paint_rect))
    {
        return;
    }

    DrawPanelRect(state.status_bar_rect, m_panel_brush.Get(), m_border_brush.Get(), 1.0f);

    std::wstring name_text;
    std::wstring size_text;
    RenderRect hovered_bounds = state.status_bar_rect;
    bool follow_tooltip = true;
    if (!TryGetHoverDisplayInfo(state, &name_text, &size_text, &hovered_bounds, &follow_tooltip))
    {
        return;
    }

    const std::wstring status_text = name_text + L" - " + size_text.substr(6);
    const RenderRect text_rect = MakeRenderRect(
        state.status_bar_rect.left + Scale(12.0f),
        state.status_bar_rect.top + Scale(kStatusBarTextVerticalPadding),
        state.status_bar_rect.right - Scale(12.0f),
        state.status_bar_rect.bottom - Scale(kStatusBarTextVerticalPadding));
    DrawTextLine(status_text, RenderTextFormat::Small, text_rect, m_text_brush.Get());
}


} // namespace swift_disk::gui










