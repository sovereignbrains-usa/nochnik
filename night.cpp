// Ночник. Тёплый свет зафиксирован (02.11.2026): R 0.72, G 0.60, B 0.50.
// Тусклость до этой точки ослабляет тот же цвет. Дальше драйвер не принимает
// канал ниже ~0.50, поэтому доп. темноту даёт прозрачная чёрная плёнка.
// Клики сквозь плёнку проходят. Подсветку монитора не трогает. Без админа.
// На выходе гамма обычная.
//
// Чистый Win32 + GDI+, без рантаймов (/MT). Интерфейс рисуется в одном окне.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using std::max;
using std::min;
#include <gdiplus.h>

#include "startup.h"
#include "version.h"

namespace G = Gdiplus;
#include "base.h"
#include "config.h"
#include "display.h"
#include "selfcheck.h"

using namespace night;

namespace
{
    const wchar_t kMainClass[] = L"NightMainWindow";
    const wchar_t kMutexName[] = L"Local\\Night.Single";

    constexpr UINT WM_TRAY = WM_APP + 1;
    constexpr UINT WM_AUTOSTATE = WM_APP + 2;
    constexpr UINT WM_SHOWME = WM_APP + 3;
    constexpr UINT WM_QUITME = WM_APP + 4;

    constexpr UINT_PTR kTimer = 1;
    constexpr int kDesignW = 420;
    constexpr int kDesignH = 460;

    enum Hit { HitNone = -1, HitSlider = 0, HitAuto = 1, HitKey0 = 2, HitExit = 5 };

    const wchar_t* const kKeyTitles[3] = {L"Светлее", L"Темнее", L"Обычный экран"};
    const wchar_t kKeyHelp[] = L"Щёлкни и нажми сочетание. Esc — отмена, Backspace — убрать.";

    // Цвета интерфейса.
    const G::Color cBg(255, 28, 28, 30);
    const G::Color cFg(255, 232, 232, 234);
    const G::Color cMuted(255, 150, 150, 156);
    const G::Color cAccent(255, 232, 176, 112);
    const G::Color cWarn(255, 255, 170, 130);

    inline COLORREF Rgb(const G::Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }

    struct App
    {
        HINSTANCE inst = nullptr;
        HWND hwnd = nullptr;
        Config cfg;
        bool live = true;
        bool allowShow = true;
        bool quitting = false;
        bool started = false;
        bool dirty = false;
        bool inTick = false;
        bool appliedOk = true;
        bool gammaOk = true;
        int dim = Look::Liked;
        int shownDim = INT_MIN;
        std::atomic<int> force{0};
        ULONGLONG lastSave = 0;
        bool heldOff = false;
        bool keyOn[3] = {};
        bool keyBusy[3] = {};
        int capture = -1;
        std::wstring keyNote;
        ULONGLONG keyNoteUntil = 0;
        bool noteWasActive = false;

        int bar = Look::Liked;
        bool drag = false;
        int hover = HitNone;
        int pressed = HitNone;
        bool trackingLeave = false;

        bool packaged = false;
        bool autoOn = false;

        double scale = 1.0;
        UINT dpi = 96;

        std::vector<Veil> veils;
        std::wstring veilKey;

        std::wstring statusText;
        G::Color statusColor = cMuted;
        std::wstring tip;
        bool trayAdded = false;
        UINT taskbarCreated = 0;

        std::unique_ptr<G::Font> fTitle, fSub, fBase, fBold, fValue, fSmall, fFoot;

        // ---------------------------------------------------------------- создание

        bool create(bool openWindow, bool isLive)
        {
            live = isLive;
            allowShow = openWindow;
            dim = cfg.dim;
            bar = dim;
            packaged = Startup::Packaged();
            autoOn = cfg.autostart;
            taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

            const wchar_t* fam = L"Segoe UI";
            auto mk = [&](float pt, bool bold) {
                return std::make_unique<G::Font>(fam, pt * 96.0f / 72.0f, bold ? G::FontStyleBold : G::FontStyleRegular, G::UnitPixel);
            };
            fTitle = mk(16, true);
            fSub = mk(9.5f, false);
            fBase = mk(10, false);
            fBold = mk(10, true);
            fValue = mk(14, true);
            fSmall = mk(8.5f, false);
            fFoot = mk(9, false);

            HICON big = LoadIconW(inst, MAKEINTRESOURCEW(1));
            HICON smallIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = &App::Proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = CreateSolidBrush(Rgb(cBg));
            wc.lpszClassName = kMainClass;
            wc.hIcon = big;
            wc.hIconSm = smallIcon;
            RegisterClassExW(&wc);

            WNDCLASSEXW vc{};
            vc.cbSize = sizeof vc;
            vc.lpfnWndProc = DefWindowProcW;
            vc.hInstance = inst;
            vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            vc.lpszClassName = kVeilClass;
            RegisterClassExW(&vc);

            hwnd = CreateWindowExW(0, kMainClass, L"Ночник", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, this);
            if (!hwnd) return false;

            dpi = GetDpiForWindow(hwnd);
            scale = dpi / 96.0;
            layoutWindow();
            // Первый ShowWindow процесса берёт nCmdShow из STARTUPINFO (при запуске из ярлыка «свёрнуто»
            // окно застревало иконкой за краем экрана). Расходуем его на скрытое окно.
            ShowWindow(hwnd, SW_HIDE);
            return true;
        }

        void start()
        {
            started = true;
            if (!packaged && cfg.autostart) SetRunKey(true);
            if (packaged) queryAutostart();
            WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
            SetTimer(hwnd, kTimer, 400, nullptr);
            registerKeys();
            applyNow(true);
            refreshTexts();
            if (allowShow) showFromTray();
        }

        void layoutWindow()
        {
            DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
            RECT r{0, 0, (LONG)std::lround(kDesignW * scale), (LONG)std::lround(kDesignH * scale)};
            AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
            int w = r.right - r.left, h = r.bottom - r.top;
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
            int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
            SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // ---------------------------------------------------------------- горячие клавиши

        Bind& keyOf(int i) { return i == 0 ? cfg.keyLighter : i == 1 ? cfg.keyDarker : cfg.keyToggle; }

        void registerKeys()
        {
            if (!live) return;
            unregisterKeys();
            for (int i = 0; i < 3; i++)
            {
                const Bind& b = keyOf(i);
                keyBusy[i] = false;
                if (b.empty()) continue;
                keyOn[i] = RegisterHotKey(hwnd, i + 1, b.mods | MOD_NOREPEAT, b.vk) != FALSE;
                keyBusy[i] = !keyOn[i];
            }
            invalidate();
        }

        void unregisterKeys()
        {
            for (int i = 0; i < 3; i++)
            {
                if (keyOn[i]) UnregisterHotKey(hwnd, i + 1);
                keyOn[i] = false;
            }
        }

        void beginCapture(int i)
        {
            if (capture == i)
            {
                endCapture();
                return;
            }
            unregisterKeys();
            capture = i;
            keyNote.clear();
            keyNoteUntil = 0;
            invalidate();
        }

        void endCapture()
        {
            if (capture < 0) return;
            capture = -1;
            registerKeys();
            invalidate();
        }

        void note(const std::wstring& text)
        {
            keyNote = text;
            keyNoteUntil = GetTickCount64() + 4000;
            invalidate();
        }

        bool noteActive() const { return !keyNote.empty() && GetTickCount64() < keyNoteUntil; }

        // Клавиша съедена режимом записи.
        void captureKey(UINT vk)
        {
            UINT mods = Bind::currentMods();
            if (vk == VK_ESCAPE && mods == 0)
            {
                endCapture();
                return;
            }
            if ((vk == VK_BACK || vk == VK_DELETE) && mods == 0)
            {
                keyOf(capture) = Bind();
                dirty = true;
                endCapture();
                return;
            }
            Bind b;
            if (!Bind::fromVk(vk, mods, b)) return;
            if (!b.safeAlone())
            {
                note(L"Нужен Ctrl, Alt или Win — иначе клавиша пропадёт при наборе текста.");
                return;
            }
            int target = capture;
            std::wstring moved;
            for (int j = 0; j < 3; j++)
            {
                if (j != target && keyOf(j).same(b))
                {
                    keyOf(j) = Bind();
                    moved = b.text() + L" было у «" + kKeyTitles[j] + L"» — там теперь пусто.";
                }
            }
            keyOf(target) = b;
            dirty = true;
            endCapture();
            if (!moved.empty()) note(moved);
            if (keyBusy[target]) note(b.text() + L" уже занято другой программой. Выбери другое.");
        }

        // ---------------------------------------------------------------- показ окна

        void showFromTray()
        {
            if (quitting) return;
            Mark(L"show");
            allowShow = true;
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else ShowWindow(hwnd, SW_SHOWNORMAL);
            // Размер «нормального» окна после странного запуска мог оказаться крошечным.
            RECT cr;
            GetClientRect(hwnd, &cr);
            if (cr.right < (LONG)std::lround(kDesignW * scale) || cr.bottom < (LONG)std::lround(kDesignH * scale)) layoutWindow();
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            RECT wr, inter;
            GetWindowRect(hwnd, &wr);
            if (!IntersectRect(&inter, &wr, &mi.rcWork)) layoutWindow();
            raiseAboveVeil();
            SetForegroundWindow(hwnd);
        }

        void hideToTray()
        {
            Mark(L"hide-to-tray");
            endCapture();
            saveNow();
            allowShow = false;
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(hwnd, SW_HIDE);
        }

        void raiseAboveVeil()
        {
            if (!IsWindowVisible(hwnd)) return;
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        void exitClean()
        {
            Mark(L"exit-clean");
            if (quitting) return;
            quitting = true;
            saveNow();
            Gamma::Identity();
            hideVeils();
            removeTray();
            DestroyWindow(hwnd);
        }

        // ---------------------------------------------------------------- яркость

        void nudge(int delta) { setDim(dim + delta); }

        void setDim(int v)
        {
            v = std::clamp(v, 0, 100);
            if (bar == v)
            {
                if (!heldOff) return;
                heldOff = false;
                applyNow(true);
                refreshTexts();
                return;
            }
            setBar(v);
        }

        void setBar(int v)
        {
            v = std::clamp(v, 0, 100);
            if (v == bar) return;
            bar = v;
            invalidate();
            dim = bar;
            heldOff = false;
            dirty = true;
            applyNow(true);
            refreshTexts();
        }

        void toggleHold()
        {
            heldOff = !heldOff;
            applyNow(true);
            refreshTexts();
        }

        int effectiveDim() const { return heldOff ? 0 : dim; }

        void applyNow(bool forced)
        {
            if (quitting) return;
            int d = effectiveDim();
            if (!forced && d == shownDim && appliedOk) return;

            double strength, veil;
            Look::Split(d, strength, veil);
            bool ok = Gamma::Apply(strength);

            shownDim = d;
            gammaOk = ok;
            appliedOk = ok || d == 0;

            if (d == 0)
            {
                hideVeils();
                return;
            }
            if (!ok)
            {
                Gamma::Identity();
                cover(d / 100.0 * Look::MaxVeil);
                appliedOk = true;
                return;
            }
            if (veil >= 0.02) cover(veil);
            else hideVeils();
        }

        static BOOL CALLBACK CollectMonitors(HMONITOR mon, HDC, LPRECT, LPARAM data)
        {
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(mon, &mi) && mi.rcMonitor.right - mi.rcMonitor.left >= 10)
                ((std::vector<RECT>*)data)->push_back(mi.rcMonitor);
            return TRUE;
        }

        void cover(double opacity)
        {
            std::vector<RECT> mons;
            EnumDisplayMonitors(nullptr, nullptr, CollectMonitors, (LPARAM)&mons);
            if (mons.empty())
            {
                hideVeils();
                return;
            }
            wchar_t head[32];
            swprintf_s(head, L"%.2f|", opacity);
            std::wstring key = head;
            for (const RECT& r : mons)
                key += std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right - r.left) + L"," +
                       std::to_wstring(r.bottom - r.top) + L";";
            if (key != veilKey || veils.size() != mons.size())
            {
                hideVeils();
                veilKey = key;
                veils.resize(mons.size());
                for (size_t i = 0; i < mons.size(); i++) veils[i].place(mons[i], opacity);
            }
            else
            {
                for (size_t i = 0; i < mons.size(); i++) veils[i].place(mons[i], opacity);
            }
            raiseAboveVeil();
        }

        void hideVeils()
        {
            for (Veil& v : veils) v.destroy();
            veils.clear();
            veilKey.clear();
        }

        // ---------------------------------------------------------------- сохранение

        void saveNow()
        {
            cfg.dim = dim;
            cfg.autostart = autoOn;
            cfg.save();
            dirty = false;
            lastSave = GetTickCount64();
        }

        void saveIfDirty()
        {
            if (!dirty) return;
            if (GetTickCount64() - lastSave < 350) return;
            saveNow();
        }

        // ---------------------------------------------------------------- автозапуск

        void queryAutostart()
        {
            HWND h = hwnd;
            std::thread([h] {
                Startup::State st = Startup::Query();
                PostMessageW(h, WM_AUTOSTATE, (WPARAM)st, 2);
            }).detach();
        }

        void setAutostart(bool on)
        {
            autoOn = on;
            cfg.autostart = on;
            dirty = true;
            if (packaged)
            {
                HWND h = hwnd;
                std::thread([h, on] {
                    Startup::State st = Startup::Set(on);
                    PostMessageW(h, WM_AUTOSTATE, (WPARAM)st, on ? 1 : 0);
                }).detach();
            }
            else SetRunKey(on);
            invalidate();
            refreshTexts();
        }

        void onAutostate(Startup::State st, int request)
        {
            if (st == Startup::State::Error)
            {
                if (request == 1)
                {
                    autoOn = false;
                    cfg.autostart = false;
                    note(L"Автозапуск не включился.");
                }
            }
            else if (request == 2 && st == Startup::State::Disabled && cfg.autostart)
            {
                // Новая установка пакета: задача запуска выключена, а в конфиге автозапуск был включён — переносим.
                setAutostart(true);
                return;
            }
            else
            {
                autoOn = st == Startup::State::Enabled;
                cfg.autostart = autoOn;
                if (request == 1 && !autoOn)
                    note(L"Автозапуск выключен в Параметрах Windows: Приложения → Автозагрузка.");
            }
            invalidate();
        }

        // ---------------------------------------------------------------- тексты

        void refreshTexts()
        {
            std::wstring text;
            G::Color col = cMuted;
            if (heldOff)
            {
                text = cfg.keyToggle.empty() ? L"Обычный экран. Двинь полосу — вернётся тёплый."
                                             : L"Обычный экран. " + cfg.keyToggle.text() + L" вернёт тёплый.";
            }
            else if (!gammaOk && dim > 0)
            {
                text = L"Драйвер не взял гамму, держу только плёнку. Тепла нет.";
                col = cWarn;
            }
            else if (dim == 0) text = L"Выключено. Экран обычный.";
            else if (dim == Look::Liked)
            {
                text = L"Тот тёплый, который зашёл.";
                col = cAccent;
            }
            else if (dim > Look::Liked)
            {
                text = L"Темнее того самого. Цвет тот же, сверху плёнка.";
                col = cAccent;
            }
            else
            {
                text = L"Светлее. Тот же цвет, слабее.";
                col = cAccent;
            }
            if (text != statusText || col.GetValue() != statusColor.GetValue())
            {
                statusText = text;
                statusColor = col;
                invalidate();
            }

            std::wstring t = heldOff ? L"Ночник — обычный экран" : L"Ночник — тусклость " + std::to_wstring(dim);
            if (t.size() > 63) t.resize(63);
            if (t != tip)
            {
                tip = t;
                updateTrayTip();
            }
        }

        void invalidate()
        {
            if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
        }

        // ---------------------------------------------------------------- трей

        void addTray()
        {
            if (!live) return;
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof nid;
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            nid.uCallbackMessage = WM_TRAY;
            nid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                          GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR);
            wcsncpy_s(nid.szTip, tip.empty() ? L"Ночник" : tip.c_str(), _TRUNCATE);
            trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            if (nid.hIcon) DestroyIcon(nid.hIcon);
        }

        void updateTrayTip()
        {
            if (!trayAdded) return;
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof nid;
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_TIP;
            wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }

        void removeTray()
        {
            if (!trayAdded) return;
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof nid;
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            trayAdded = false;
        }

        void trayMenu()
        {
            enum { Open = 1, Level, Lighter, Darker, Liked, Off, Auto, Exit };
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, Open, L"Открыть");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | MF_GRAYED, Level, (L"Тусклость: " + std::to_wstring(dim)).c_str());
            AppendMenuW(m, MF_STRING, Lighter, L"Светлее");
            AppendMenuW(m, MF_STRING, Darker, L"Темнее");
            AppendMenuW(m, MF_STRING, Liked, L"Как было");
            AppendMenuW(m, MF_STRING, Off, L"Выключить");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (autoOn ? MF_CHECKED : 0), Auto, L"Запускать вместе с Windows");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, Exit, L"Выйти");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(m);
            switch (cmd)
            {
            case Open: showFromTray(); break;
            case Lighter: nudge(-5); break;
            case Darker: nudge(5); break;
            case Liked: setDim(Look::Liked); break;
            case Off: setDim(0); break;
            case Auto: setAutostart(!autoOn); break;
            case Exit: exitClean(); break;
            }
        }

        // ---------------------------------------------------------------- геометрия (в 96-dpi единицах)

        static RECT KeyRect(int i) { return RECT{190, 286 + i * 34, 400, 286 + i * 34 + 28}; }
        static constexpr RECT SliderRect{10, 108, 410, 144};
        static constexpr RECT AutoRect{20, 220, 300, 246};
        static constexpr RECT ExitRect{320, 424, 400, 448};

        static bool In(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

        int hitTest(int px, int py) const
        {
            int x = (int)std::lround(px / scale), y = (int)std::lround(py / scale);
            if (In(SliderRect, x, y)) return HitSlider;
            if (In(AutoRect, x, y)) return HitAuto;
            for (int i = 0; i < 3; i++)
                if (In(KeyRect(i), x, y)) return HitKey0 + i;
            if (In(ExitRect, x, y)) return HitExit;
            return HitNone;
        }

        static int SliderValueAt(double x)
        {
            double x1 = SliderRect.left + 10, x2 = SliderRect.right - 10;
            double t = std::clamp((x - x1) / (x2 - x1), 0.0, 1.0);
            return (int)std::nearbyint(t * 100.0);
        }

        // ---------------------------------------------------------------- рисование

        static void T(G::Graphics& g, const std::wstring& s, G::Font& f, float x, float y, float w, float h, const G::Color& c,
                      G::StringAlignment ha = G::StringAlignmentNear, G::StringAlignment va = G::StringAlignmentNear)
        {
            G::StringFormat fmt(G::StringFormat::GenericTypographic());
            fmt.SetAlignment(ha);
            fmt.SetLineAlignment(va);
            fmt.SetTrimming(G::StringTrimmingNone);
            G::SolidBrush br(c);
            g.DrawString(s.c_str(), (INT)s.size(), &f, G::RectF(x, y, w, h), &fmt, &br);
        }

        void paint(HDC hdc, int w, int h)
        {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                G::Graphics g(mem);
                g.SetSmoothingMode(G::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(G::TextRenderingHintClearTypeGridFit);
                g.ScaleTransform((G::REAL)scale, (G::REAL)scale);
                g.Clear(cBg);
                draw(g);
            }
            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }

        void draw(G::Graphics& g)
        {
            const auto Near = G::StringAlignmentNear;
            const auto Center = G::StringAlignmentCenter;
            const auto Far = G::StringAlignmentFar;

            T(g, L"Ночник", *fTitle, 20, 14, 380, 28, cFg);
            T(g, L"Тёплый свет не трогаю. Крутится тусклость.", *fSub, 20, 44, 380, 22, cMuted);
            T(g, L"Тусклость", *fBase, 20, 78, 180, 24, cFg);
            T(g, std::to_wstring(dim), *fValue, 200, 74, 200, 28, cAccent, Far, Center);

            // Полоса.
            {
                float x1 = SliderRect.left + 10.0f, x2 = SliderRect.right - 10.0f;
                float cy = SliderRect.top + (SliderRect.bottom - SliderRect.top) / 2.0f;
                G::SolidBrush track(G::Color(255, 58, 58, 62));
                g.FillRectangle(&track, x1, cy - 2, x2 - x1, 4.0f);
                float tx = x1 + (x2 - x1) * (bar / 100.0f);
                G::SolidBrush fill(cAccent);
                g.FillRectangle(&fill, x1, cy - 2, std::max(1.0f, tx - x1), 4.0f);
                g.FillEllipse(&fill, tx - 7.0f, cy - 7.0f, 14.0f, 14.0f);
                float mx = x1 + (x2 - x1) * (Look::Liked / 100.0f);
                G::Pen mark(G::Color(255, 220, 220, 224), 1.5f);
                g.DrawLine(&mark, mx, cy - 11, mx, cy - 6);
            }
            T(g, L"светлее", *fSmall, 20, 146, 160, 18, cMuted);
            T(g, L"темнее", *fSmall, 240, 146, 160, 18, cMuted, Far);

            T(g, statusText, *fSub, 20, 172, 380, 44, statusColor);

            // Галка автозапуска.
            {
                bool hov = hover == HitAuto;
                float bx = 20, by = 224.5f, bs = 16;
                G::SolidBrush bg(hov ? G::Color(255, 52, 52, 58) : G::Color(255, 40, 40, 44));
                g.FillRectangle(&bg, bx, by, bs, bs);
                G::Pen border(autoOn ? cAccent : G::Color(255, 110, 110, 116), 1.0f);
                g.DrawRectangle(&border, bx + 0.5f, by + 0.5f, bs - 1, bs - 1);
                if (autoOn)
                {
                    G::SolidBrush on(cAccent);
                    g.FillRectangle(&on, bx + 3, by + 3, bs - 6, bs - 6);
                }
                T(g, L"Запускать вместе с Windows", *fBase, 44, 222, 260, 22, cFg, Near, Center);
            }

            T(g, L"Горячие клавиши", *fBold, 20, 258, 380, 22, cFg);
            for (int i = 0; i < 3; i++)
            {
                RECT r = KeyRect(i);
                T(g, kKeyTitles[i], *fBase, 20, (float)r.top + 4, 170, 22, cFg);

                std::wstring text;
                G::Color fg = cFg;
                if (capture == i)
                {
                    text = L"Нажми сочетание…";
                    fg = cAccent;
                }
                else
                {
                    const Bind& b = keyOf(i);
                    text = b.text();
                    if (b.empty()) fg = cMuted;
                    else if (keyBusy[i])
                    {
                        text += L" — занято";
                        fg = cWarn;
                    }
                }
                bool isDown = pressed == HitKey0 + i && hover == pressed;
                G::Color fillc = isDown ? G::Color(255, 60, 60, 66) : hover == HitKey0 + i ? G::Color(255, 52, 52, 58) : G::Color(255, 40, 40, 44);
                G::Color bord = capture == i ? cAccent : G::Color(255, 70, 70, 76);
                G::SolidBrush fb(fillc);
                float bw = (float)(r.right - r.left), bh = (float)(r.bottom - r.top);
                g.FillRectangle(&fb, (float)r.left, (float)r.top, bw, bh);
                G::Pen bp(bord, 1.0f);
                g.DrawRectangle(&bp, r.left + 0.5f, r.top + 0.5f, bw - 1, bh - 1);
                T(g, text, *fBase, (float)r.left, (float)r.top, bw, bh, fg, Center, Center);
            }

            bool shown = noteActive();
            T(g, shown ? keyNote : std::wstring(kKeyHelp), *fSmall, 20, 390, 380, 34, shown ? cWarn : cMuted);

            T(g, L"Крестик прячет в трей.", *fFoot, 20, 428, 220, 22, cMuted);
            {
                const wchar_t* s = L"Выйти";
                G::Font f(L"Segoe UI", 10.0f * 96.0f / 72.0f, hover == HitExit ? G::FontStyleUnderline : G::FontStyleRegular, G::UnitPixel);
                T(g, s, f, 320, 426, 80, 22, hover == HitExit ? G::Color(255, 255, 255, 255) : cAccent, Far, Near);
            }
        }

        // ---------------------------------------------------------------- таймер и события

        void onTick()
        {
            if (inTick || quitting) return;
            inTick = true;
            saveIfDirty();
            bool f = force.exchange(0) != 0;
            applyNow(f);
            refreshTexts();
            bool na = noteActive();
            if (na != noteWasActive)
            {
                noteWasActive = na;
                invalidate();
            }
            inTick = false;
        }

        // ---------------------------------------------------------------- оконная процедура

        static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
        {
            App* self;
            if (msg == WM_NCCREATE)
            {
                self = (App*)((CREATESTRUCTW*)lp)->lpCreateParams;
                self->hwnd = h;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            }
            else self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (!self) return DefWindowProcW(h, msg, wp, lp);
            return self->handle(h, msg, wp, lp);
        }

        LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (taskbarCreated && msg == taskbarCreated)
            {
                trayAdded = false;
                addTray();
                return 0;
            }
            switch (msg)
            {
            case WM_CREATE:
            {
                BOOL on = TRUE;
                DwmSetWindowAttribute(h, 20, &on, sizeof on);
                COLORREF cap = Rgb(cBg), txt = Rgb(cFg);
                DwmSetWindowAttribute(h, 35, &cap, sizeof cap);
                DwmSetWindowAttribute(h, 36, &txt, sizeof txt);
                if (live) addTray();
                return 0;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(h, &ps);
                RECT cr;
                GetClientRect(h, &cr);
                paint(hdc, cr.right, cr.bottom);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_DPICHANGED:
            {
                dpi = HIWORD(wp);
                scale = dpi / 96.0;
                const RECT* r = (const RECT*)lp;
                SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                invalidate();
                return 0;
            }
            case WM_HOTKEY:
            {
                int id = (int)wp - 1;
                if (id == 0) nudge(-5);
                else if (id == 1) nudge(5);
                else if (id == 2) toggleHold();
                return 0;
            }
            case WM_TIMER:
                if (wp == kTimer) onTick();
                return 0;
            case WM_TRAY:
            {
                UINT ev = LOWORD(lp);
                if (ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == WM_LBUTTONUP) showFromTray();
                else if (ev == WM_CONTEXTMENU || ev == WM_RBUTTONUP) trayMenu();
                return 0;
            }
            case WM_SHOWME:
                showFromTray();
                return 0;
            case WM_QUITME:
                exitClean();
                return 0;
            case WM_AUTOSTATE:
                onAutostate((Startup::State)wp, (int)lp);
                return 0;
            case WM_DISPLAYCHANGE:
            case WM_WTSSESSION_CHANGE:
                force = 1;
                return 0;
            case WM_POWERBROADCAST:
                force = 1;
                return TRUE;
            case WM_QUERYENDSESSION: return TRUE;
            case WM_ENDSESSION:
                if (wp)
                {
                    saveNow();
                    Gamma::Identity();
                }
                return 0;
            case WM_CLOSE:
                if (!quitting)
                {
                    Mark(L"closing user");
                    hideToTray();
                    return 0;
                }
                break;
            case WM_SIZE:
                if (wp == SIZE_MINIMIZED) hideToTray();
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) endCapture();
                return 0;
            case WM_DESTROY:
                KillTimer(h, kTimer);
                unregisterKeys();
                WTSUnRegisterSessionNotification(h);
                if (started) Gamma::Identity();
                hideVeils();
                removeTray();
                PostQuitMessage(0);
                return 0;

            case WM_SYSKEYDOWN:
            case WM_KEYDOWN:
                if (capture >= 0)
                {
                    captureKey((UINT)wp);
                    return 0;
                }
                if (msg == WM_KEYDOWN)
                {
                    int step = GetKeyState(VK_SHIFT) < 0 ? 5 : 1;
                    switch (wp)
                    {
                    case VK_LEFT: nudge(-step); return 0;
                    case VK_RIGHT: nudge(step); return 0;
                    case VK_HOME: setDim(0); return 0;
                    case VK_END: setDim(100); return 0;
                    case VK_ESCAPE: hideToTray(); return 0;
                    }
                }
                break;
            case WM_SYSCHAR:
            case WM_CHAR:
                if (capture >= 0) return 0;
                break;
            case WM_SYSCOMMAND:
                if (capture >= 0 && (wp & 0xFFF0) == SC_KEYMENU) return 0;
                break;

            case WM_MOUSEWHEEL:
                nudge(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT)
                {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(h, &pt);
                    if (hitTest(pt.x, pt.y) != HitNone)
                    {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
                break;
            case WM_LBUTTONDOWN:
            {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                int hit = hitTest(x, y);
                SetFocus(h);
                SetCapture(h);
                pressed = hit;
                if (hit == HitSlider)
                {
                    drag = true;
                    setBar(SliderValueAt(x / scale));
                }
                invalidate();
                return 0;
            }
            case WM_MOUSEMOVE:
            {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                if (!trackingLeave)
                {
                    TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, h, 0};
                    TrackMouseEvent(&te);
                    trackingLeave = true;
                }
                int hit = hitTest(x, y);
                if (hit != hover)
                {
                    hover = hit;
                    invalidate();
                }
                if (drag) setBar(SliderValueAt(x / scale));
                return 0;
            }
            case WM_MOUSELEAVE:
                trackingLeave = false;
                if (hover != HitNone)
                {
                    hover = HitNone;
                    invalidate();
                }
                return 0;
            case WM_LBUTTONUP:
            {
                int hit = hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                int was = pressed;
                pressed = HitNone;
                drag = false;
                ReleaseCapture();
                if (hit != HitNone && hit == was)
                {
                    if (hit == HitAuto) setAutostart(!autoOn);
                    else if (hit >= HitKey0 && hit < HitKey0 + 3) beginCapture(hit - HitKey0);
                    else if (hit == HitExit) exitClean();
                }
                invalidate();
                return 0;
            }
            }
            return DefWindowProcW(h, msg, wp, lp);
        }
    };

    bool HasArg(int argc, wchar_t** argv, const wchar_t* name)
    {
        for (int i = 1; i < argc; i++)
            if (_wcsicmp(argv[i], name) == 0) return true;
        return false;
    }

    std::wstring ArgAfter(int argc, wchar_t** argv, const wchar_t* name)
    {
        for (int i = 1; i + 1 < argc; i++)
            if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
        return L"";
    }

    std::wstring TempFile(const wchar_t* name)
    {
        wchar_t buf[MAX_PATH + 1];
        DWORD n = GetTempPathW((DWORD)std::size(buf), buf);
        return std::wstring(buf, n) + name;
    }

    LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
    {
        wchar_t line[128];
        swprintf_s(line, L"crash code=%08lX", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0UL);
        Mark(line);
        Gamma::IdentityUnlocked(); // замок гаммы мог остаться у упавшего потока
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Тёмное контекстное меню трея. Недокументированные ординалы uxtheme (135/136), без них меню просто светлое.
    void DarkMenus()
    {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (!ux) return;
        using SetMode = int(WINAPI*)(int);
        using Flush = void(WINAPI*)();
        auto setMode = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (setMode) setMode(2);
        if (flush) flush();
    }

    HWND FindMain(int tries)
    {
        for (int i = 0; i < tries; i++)
        {
            HWND w = FindWindowW(kMainClass, nullptr);
            if (w) return w;
            Sleep(100);
        }
        return nullptr;
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (HasArg(argc, argv, L"--check"))
    {
        std::wstring out = ArgAfter(argc, argv, L"--check");
        if (out.empty() || out.rfind(L"--", 0) == 0) out = TempFile(L"night-check.txt");
        return Check(out);
    }

    if (HasArg(argc, argv, L"--reset"))
    {
        HWND w = FindMain(1);
        if (w) PostMessageW(w, WM_QUITME, 0, 0);
        else Gamma::Identity();
        return 0;
    }

    SetUnhandledExceptionFilter(CrashFilter);

    bool checkUi = HasArg(argc, argv, L"--check-ui");
    bool background = HasArg(argc, argv, L"--background");
    if (!checkUi && !background)
    {
        int kind = Startup::ActivationKind();
        if (kind >= 0) Mark(L"activation-kind " + std::to_wstring(kind));
        if (Startup::LaunchedByStartupTask()) background = true;
    }

    HANDLE mutex = nullptr;
    if (!checkUi)
    {
        mutex = CreateMutexW(nullptr, FALSE, kMutexName);
        DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        {
            HWND w = FindMain(20);
            AllowSetForegroundWindow(ASFW_ANY);
            if (w) PostMessageW(w, WM_SHOWME, 0, 0);
            CloseHandle(mutex);
            return 0;
        }
    }

    DarkMenus();

    G::GdiplusStartupInput gin;
    ULONG_PTR gtoken = 0;
    G::GdiplusStartup(&gtoken, &gin, nullptr);

    int code = 0;
    {
        App app;
        app.inst = inst;
        app.cfg = checkUi ? Config() : Config::load();
        if (!app.create(!background && !checkUi, !checkUi))
        {
            Mark(L"window-create-failed");
            code = 1;
        }
        else if (checkUi)
        {
            RECT cr;
            GetClientRect(app.hwnd, &cr);
            char buf[96];
            snprintf(buf, sizeof buf, "ui-ok %llu %ldx%ld", (unsigned long long)(UINT_PTR)app.hwnd, cr.right, cr.bottom);
            WriteFileUtf8(TempFile(L"night-ui-check.txt"), buf, false);
            DestroyWindow(app.hwnd);
        }
        else
        {
            app.start();
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Gamma::Identity();
            Mark(L"process-exit");
        }
    }
    G::GdiplusShutdown(gtoken);
    if (mutex)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}