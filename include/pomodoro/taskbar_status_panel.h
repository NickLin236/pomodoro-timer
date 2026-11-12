#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace pomodoro {

inline constexpr UINT taskbarLayoutChangedMessage = WM_APP + 92;

// A full-size panel in measured free taskbar space. Visible widget and button
// bounds are obstacles; if no safe gap can be measured, the panel stays hidden.
// It never moves or resizes Explorer or neighboring applications.
class TaskbarStatusPanel final {
public:
    // The optional explicit host lets native tests exercise placement without
    // changing the user's Explorer taskbar. Production uses Shell_TrayWnd.
    TaskbarStatusPanel(HINSTANCE instance, HWND owner, UINT callbackMessage,
                       HWND taskbarHost = nullptr);
    ~TaskbarStatusPanel();
    TaskbarStatusPanel(const TaskbarStatusPanel&) = delete;
    TaskbarStatusPanel& operator=(const TaskbarStatusPanel&) = delete;

    void update(std::wstring firstLine, std::wstring secondLine, bool visible);
    void hide();
    void explorerRestarted();
    void reposition();
    [[nodiscard]] bool available() const noexcept { return layoutAvailable_; }
    [[nodiscard]] const std::wstring& lastError() const noexcept { return error_; }
    [[nodiscard]] HWND handle() const noexcept { return window_; }

private:
    static LRESULT CALLBACK windowProcedure(HWND, UINT, WPARAM, LPARAM);
    bool create(HWND host);
    bool locate(HWND& host, RECT& hostBounds);
    void paint();
    void updateFont(UINT dpi, int height);
    void updateColors(HWND host, const RECT& bounds);
    void fail(const wchar_t* error);

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    UINT callbackMessage_ = 0;
    HWND explicitHost_ = nullptr;
    HWND host_ = nullptr;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 0;
    int fontHeight_ = 0;
    bool requestedVisible_ = false;
    bool layoutAvailable_ = false;
    bool positioning_ = false;
    bool layoutTimer_ = false;
    ULONGLONG lastLayoutCheck_ = 0;
    std::wstring firstLine_;
    std::wstring secondLine_;
    std::wstring error_;
    COLORREF background_ = RGB(32, 32, 32);
    COLORREF foreground_ = RGB(245, 245, 245);
};

} // namespace pomodoro
#endif
