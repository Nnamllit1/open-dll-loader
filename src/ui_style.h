#pragma once
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>

namespace ui {
inline constexpr COLORREF background = RGB(244, 246, 250), paper = RGB(255, 255, 255);
inline constexpr COLORREF sidebar = RGB(18, 29, 45), field = RGB(30, 44, 63);
inline constexpr COLORREF ink = RGB(26, 40, 60), muted = RGB(107, 120, 139), line = RGB(225, 231, 239);
inline constexpr COLORREF accent = RGB(26, 108, 230), green = RGB(14, 132, 108), red = RGB(184, 54, 70);
inline UINT dpi = 96;
inline int px(int value) { return MulDiv(value, static_cast<int>(dpi), 96); }
inline HFONT body{}, caption{}, strong{}, title{}, mono{}, brand{};
inline HBRUSH paperBrush{}, sidebarBrush{}, fieldBrush{};
inline HFONT font(int height, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}
inline void destroy_fonts() {
    for (auto f : {body, caption, strong, title, mono, brand}) if (f) DeleteObject(f);
}
inline void create_fonts(UINT value) {
    destroy_fonts(); dpi = value;
    body = font(14, FW_NORMAL); caption = font(12, FW_NORMAL); strong = font(14, FW_SEMIBOLD);
    title = font(27, FW_SEMIBOLD); mono = font(12, FW_NORMAL, L"Consolas"); brand = font(18, FW_SEMIBOLD);
    if (!paperBrush) {
        paperBrush = CreateSolidBrush(paper); sidebarBrush = CreateSolidBrush(sidebar); fieldBrush = CreateSolidBrush(field);
    }
}
inline void fill(HDC dc, RECT rect, COLORREF color) {
    auto brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}
inline void card(HDC dc, RECT rect, COLORREF color = paper, COLORREF border = line, int radius = 12) {
    auto brush = CreateSolidBrush(color); auto pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(radius), px(radius));
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}
inline void text(HDC dc, const std::wstring& value, RECT rect, HFONT f, COLORREF color, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, f); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, flags); SelectObject(dc, old);
}
inline RECT rect(int x, int y, int width, int height) { return {px(x), px(y), px(x + width), px(y + height)}; }
inline void move(HWND h, int x, int y, int width, int height) { MoveWindow(h, px(x), px(y), px(std::max(1, width)), px(std::max(1, height)), TRUE); }
inline LRESULT CALLBACK button_events(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE && !GetPropW(h, L"odl.hover")) {
        SetPropW(h, L"odl.hover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0}; TrackMouseEvent(&track); InvalidateRect(h, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(h, L"odl.hover"); InvalidateRect(h, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemovePropW(h, L"odl.hover"); RemoveWindowSubclass(h, button_events, 1);
    }
    return DefSubclassProc(h, message, w, l);
}
inline void button(const DRAWITEMSTRUCT& item, bool primary, bool dark) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0, down = (item.itemState & ODS_SELECTED) != 0;
    const bool hover = GetPropW(item.hwndItem, L"odl.hover") != nullptr;
    COLORREF bg = dark ? field : paper, fg = dark ? RGB(224, 233, 245) : ink, border = dark ? field : line;
    if (primary) { bg = accent; fg = paper; border = accent; }
    if (hover && !disabled) bg = primary ? RGB(20, 90, 198) : dark ? RGB(42, 59, 82) : RGB(239, 245, 255);
    if (down && !disabled) bg = primary ? RGB(16, 75, 168) : RGB(218, 230, 247);
    if (disabled) { bg = dark ? field : RGB(236, 240, 246); fg = RGB(150, 161, 177); border = bg; }
    fill(item.hDC, item.rcItem, dark ? sidebar : background);
    RECT box = item.rcItem; --box.right; --box.bottom; card(item.hDC, box, bg, border, 8);
    wchar_t label[128]{}; GetWindowTextW(item.hwndItem, label, 128);
    text(item.hDC, label, box, strong, fg, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (item.itemState & ODS_FOCUS) { InflateRect(&box, -px(4), -px(4)); DrawFocusRect(item.hDC, &box); }
}
inline void destroy() {
    destroy_fonts(); DeleteObject(paperBrush); DeleteObject(sidebarBrush); DeleteObject(fieldBrush);
}
}
