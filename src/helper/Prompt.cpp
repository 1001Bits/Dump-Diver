#include "helper/Prompt.h"

#include "helper/Util.h"

#include <objidl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Gdiplus
{
    using std::max;
    using std::min;
}
#pragma warning(push)
#pragma warning(disable : 4458)
#include <gdiplus.h>
#pragma warning(pop)

namespace fs = std::filesystem;

namespace dd::helper
{
    namespace
    {
        std::atomic<bool> g_open{ false };  // one prompt at a time

        constexpr int      kQuit = 101;
        constexpr int      kWait = 102;
        constexpr UINT_PTR kRecoveryTimer = 1;
        constexpr wchar_t kClass[] = L"DumpDiverFreezePrompt";
        constexpr wchar_t kQuestion[] = L"It looks like your game is frozen. Quit game and open freeze report?";

        struct State
        {
            DWORD                           pid = 0;
            fs::path                        report;
            std::unique_ptr<Gdiplus::Image> image;
            int                             imageWidth = 0;
            int                             imageHeight = 0;
            HFONT                           font = nullptr;
            int                             dpi = 96;
            StillStuck                      stillStuck;
        };

        int Scale(const State& state, int value) { return MulDiv(value, state.dpi, 96); }

        // The report opens first: once the game is gone the helper exits, and this thread with it.
        void QuitAndOpen(const State& state)
        {
            ShellExecuteW(nullptr, L"open", state.report.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (HANDLE game = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, state.pid)) {
                TerminateProcess(game, 1);
                WaitForSingleObject(game, 5000);
                CloseHandle(game);
            }
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (message) {
            case WM_CREATE: {
                state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
                RECT client{};
                GetClientRect(hwnd, &client);
                const int margin = Scale(*state, 20);
                const int buttonHeight = Scale(*state, 32);
                const int buttonY = client.bottom - margin - buttonHeight;
                const int waitWidth = Scale(*state, 130);
                const int quitWidth = Scale(*state, 230);
                // Keep waiting is the default and has the focus: a key meant for the game (Enter, Space)
                // must never quit it.
                HWND quit = CreateWindowExW(0, L"BUTTON", L"Quit game and open report", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                            client.right - margin - waitWidth - Scale(*state, 10) - quitWidth, buttonY, quitWidth,
                                            buttonHeight, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQuit)), nullptr, nullptr);
                HWND wait = CreateWindowExW(0, L"BUTTON", L"Keep waiting", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                            client.right - margin - waitWidth, buttonY, waitWidth, buttonHeight, hwnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWait)), nullptr, nullptr);
                SendMessageW(quit, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
                SendMessageW(wait, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
                SetFocus(wait);
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == kQuit) {
                    ShowWindow(hwnd, SW_HIDE);
                    QuitAndOpen(*state);
                    DestroyWindow(hwnd);
                } else if (LOWORD(wParam) == kWait || LOWORD(wParam) == IDCANCEL) {  // IDCANCEL: Esc
                    DestroyWindow(hwnd);
                }
                return 0;
            case WM_TIMER:
                // The game is back: the question no longer applies.
                if (wParam == kRecoveryTimer && state->stillStuck && !state->stillStuck())
                    DestroyWindow(hwnd);
                return 0;
            case WM_PRINTCLIENT:
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC         dc = message == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wParam);
                RECT        client{};
                GetClientRect(hwnd, &client);
                FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
                const int margin = Scale(*state, 20);
                int       y = margin;
                if (state->image) {
                    Gdiplus::Graphics graphics(dc);
                    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    const int x = (client.right - state->imageWidth) / 2;
                    graphics.DrawImage(state->image.get(), x, y, state->imageWidth, state->imageHeight);
                    y += state->imageHeight + Scale(*state, 14);
                }
                HGDIOBJ old = SelectObject(dc, state->font);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                RECT text{ margin, y, client.right - margin, client.bottom - Scale(*state, 70) };
                DrawTextW(dc, kQuestion, -1, &text, DT_CENTER | DT_WORDBREAK);
                SelectObject(dc, old);
                if (message == WM_PAINT)
                    EndPaint(hwnd, &paint);
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
        }

        bool PngEncoder(CLSID& clsid)
        {
            UINT count = 0, size = 0;
            Gdiplus::GetImageEncodersSize(&count, &size);
            if (!size)
                return false;
            std::vector<std::uint8_t> buffer(size);
            auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
            Gdiplus::GetImageEncoders(count, size, encoders);
            for (UINT i = 0; i < count; ++i) {
                if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                    clsid = encoders[i].Clsid;
                    return true;
                }
            }
            return false;
        }

        // The window as it would look, drawn into a PNG without ever showing it.
        bool SavePreview(HWND hwnd, const fs::path& out)
        {
            RECT bounds{};
            GetWindowRect(hwnd, &bounds);
            const int width = bounds.right - bounds.left;
            const int height = bounds.bottom - bounds.top;
            HDC     screen = GetDC(nullptr);
            HDC     memory = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
            HGDIOBJ old = SelectObject(memory, bitmap);
            SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
            SelectObject(memory, old);
            bool saved = false;
            CLSID png{};
            if (PngEncoder(png)) {
                Gdiplus::Bitmap image(bitmap, nullptr);
                saved = image.Save(out.c_str(), &png, nullptr) == Gdiplus::Ok;
            }
            DeleteObject(bitmap);
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);
            return saved;
        }

        void Run(DWORD pid, const fs::path& report, const fs::path& image, const fs::path& preview, const StillStuck& stillStuck)
        {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
            Gdiplus::GdiplusStartupInput input;
            ULONG_PTR                    token = 0;
            const bool                   gdiplus = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;

            {
                State state;
                state.pid = pid;
                state.report = report;
                state.stillStuck = stillStuck;
                state.dpi = static_cast<int>(GetDpiForSystem());
                std::error_code ec;
                if (gdiplus && !image.empty() && fs::is_regular_file(image, ec)) {
                    auto picture = std::make_unique<Gdiplus::Image>(image.c_str());
                    if (picture->GetLastStatus() == Gdiplus::Ok && picture->GetWidth() && picture->GetHeight()) {
                        // at most 360 x 240 (at 96 DPI), aspect kept
                        const double fit = std::min({ 1.0, Scale(state, 360) / static_cast<double>(picture->GetWidth()),
                                                      Scale(state, 240) / static_cast<double>(picture->GetHeight()) });
                        state.imageWidth = static_cast<int>(picture->GetWidth() * fit);
                        state.imageHeight = static_cast<int>(picture->GetHeight() * fit);
                        state.image = std::move(picture);
                    }
                }
                state.font = CreateFontW(-Scale(state, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = &WindowProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.hIcon = LoadIconW(nullptr, IDI_WARNING);
                wc.lpszClassName = kClass;
                RegisterClassExW(&wc);

                const int width = Scale(state, 460);
                const int height = Scale(state, 170) + (state.image ? state.imageHeight + Scale(state, 14) : 0);
                RECT      frame{ 0, 0, width, height };
                const DWORD style = WS_CAPTION | WS_SYSMENU;
                AdjustWindowRectEx(&frame, style, FALSE, WS_EX_TOPMOST);
                const int outerWidth = frame.right - frame.left;
                const int outerHeight = frame.bottom - frame.top;
                const int x = (GetSystemMetrics(SM_CXSCREEN) - outerWidth) / 2;
                const int y = (GetSystemMetrics(SM_CYSCREEN) - outerHeight) / 2;
                HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Dump Diver", style, x, y, outerWidth, outerHeight, nullptr, nullptr,
                                            wc.hInstance, &state);
                if (hwnd && !preview.empty()) {
                    SavePreview(hwnd, preview);
                    DestroyWindow(hwnd);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                } else if (hwnd) {
                    ShowWindow(hwnd, SW_SHOWNORMAL);
                    SetForegroundWindow(hwnd);
                    FLASHWINFO flash{ sizeof(flash), hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
                    FlashWindowEx(&flash);
                    if (state.stillStuck)
                        SetTimer(hwnd, kRecoveryTimer, 250, nullptr);

                    // The prompt goes away by itself when the game ends.
                    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
                    for (bool running = true; running;) {
                        const DWORD wait = MsgWaitForMultipleObjects(game ? 1 : 0, &game, FALSE, INFINITE, QS_ALLINPUT);
                        if (game && wait == WAIT_OBJECT_0) {
                            DestroyWindow(hwnd);
                        }
                        MSG message{};
                        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                            if (message.message == WM_QUIT) {
                                running = false;
                                break;
                            }
                            if (!IsDialogMessageW(hwnd, &message)) {
                                TranslateMessage(&message);
                                DispatchMessageW(&message);
                            }
                        }
                    }
                    if (game)
                        CloseHandle(game);
                }
                UnregisterClassW(kClass, wc.hInstance);
                if (state.font)
                    DeleteObject(state.font);
            }
            if (gdiplus)
                Gdiplus::GdiplusShutdown(token);
        }
    }

    void ShowFreezePrompt(DWORD pid, const fs::path& report, const fs::path& image, StillStuck stillStuck)
    {
        bool expected = false;
        if (!g_open.compare_exchange_strong(expected, true))
            return;  // one is already up
        std::thread([pid, report, image, stillStuck = std::move(stillStuck)] {
            Run(pid, report, image, fs::path(), stillStuck);
            g_open = false;
        }).detach();
    }

    void RunFreezePrompt(DWORD pid, const fs::path& report, const fs::path& image, const StillStuck& stillStuck)
    {
        Run(pid, report, image, fs::path(), stillStuck);
    }

    void PreviewFreezePrompt(const fs::path& image, const fs::path& out)
    {
        Run(0, fs::path(), image, out, nullptr);
    }
}
