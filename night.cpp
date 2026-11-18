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
#include "layout.h"
#include "selfcheck.h"

using namespace night;

// Номера ресурсов RCDATA в night.rc.
constexpr int kFontRegular = 101, kFontBold = 102;

namespace
{
    const wchar_t kMainClass[] = L"NightMainWindow";
    const wchar_t kMutexName[] = L"Local\\Night.Single";

    constexpr UINT WM_TRAY = WM_APP + 1;
    constexpr UINT WM_AUTOSTATE = WM_APP + 2;
    constexpr UINT WM_SHOWME = WM_APP + 3;
    constexpr UINT WM_QUITME = WM_APP + 4;

    constexpr UINT_PTR kTimer = 1;
    constexpr ULONGLONG kApplyGap = 50;     // ползунок не дёргает гамму чаще, мс
    constexpr ULONGLONG kGammaRetry = 2000; // повтор, если драйвер отверг рампу, мс

    enum Hit { HitNone = -1, HitSlider = 0, HitAuto = 1, HitKey0 = 2 };

    const wchar_t* const kKeyTitles[3] = {L"Светлее", L"Темнее", L"Обычный экран"};
    const wchar_t kKeyHelp[] = L"Щёлкните по полю и нажмите сочетание.\nEsc — отмена, Backspace — убрать.";

    // Цвета интерфейса.
    const G::Color cBg(255, 28, 28, 30);
    const G::Color cCard(255, 38, 38, 42);
    const G::Color cCardHot(255, 44, 44, 49);
    const G::Color cCardLine(255, 58, 58, 64);
    const G::Color cTrack(255, 66, 66, 72);
    const G::Color cThumb(255, 240, 240, 242);
    const G::Color cBtn(255, 50, 50, 56);
    const G::Color cBtnHot(255, 62, 62, 69);
    const G::Color cBtnDown(255, 44, 44, 50);
    const G::Color cBtnLine(255, 78, 78, 86);
    const G::Color cSwitchOff(255, 140, 140, 146);
    const G::Color cSwitchOffHot(255, 190, 190, 196);
    const G::Color cFgBright(255, 255, 255, 255);
    const G::Color cRing(255, 232, 232, 234);
    const G::Color cAccentHot(255, 244, 196, 136);
    const G::Color cFg(255, 232, 232, 234);
    const G::Color cMuted(255, 150, 150, 156);
    const G::Color cHint(255, 188, 188, 194);
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
        ULONGLONG lastApply = 0;
        ULONGLONG gammaRetryAt = 0;
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
        int focus = HitNone;         // элемент с клавиатурным фокусом (порядок Tab = номера Hit)
        bool focusVisible = false;   // кольцо рисуем только после Tab, а не после мыши
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

        std::unique_ptr<G::PrivateFontCollection> fontColl; // шрифты Onest из ресурсов; живёт дольше шрифтов ниже
        std::unique_ptr<G::Font> fSub, fBase, fBold, fValue, fSmall, fFoot;

        // Onest (SIL OFL) зашит в exe: Regular и Bold. Нет ресурса или не прочитался — остаётся Segoe UI.
        std::unique_ptr<G::FontFamily> loadFamily()
        {
            fontColl = std::make_unique<G::PrivateFontCollection>();
            for (int id : {kFontRegular, kFontBold})
            {
                HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
                HGLOBAL h = r ? LoadResource(inst, r) : nullptr;
                const void* p = h ? LockResource(h) : nullptr;
                if (p) fontColl->AddMemoryFont(p, (INT)SizeofResource(inst, r));
            }
            if (fontColl->GetFamilyCount() > 0)
            {
                G::FontFamily found[1];
                INT n = 0;
                if (fontColl->GetFamilies(1, found, &n) == G::Ok && n == 1) return std::unique_ptr<G::FontFamily>(found[0].Clone());
            }
            Mark(L"font: Onest не загрузился, берём Segoe UI");
            return std::make_unique<G::FontFamily>(L"Segoe UI");
        }

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

            const std::unique_ptr<G::FontFamily> fam = loadFamily();
            auto mk = [&](float pt, bool bold, bool underline = false) {
                int style = (bold ? G::FontStyleBold : G::FontStyleRegular) | (underline ? G::FontStyleUnderline : 0);
                return std::make_unique<G::Font>(fam.get(), pt * 96.0f / 72.0f, style, G::UnitPixel);
            };
            fSub = mk(9.5f, false);
            fBase = mk(10, false);
            fBold = mk(10, true);
            fValue = mk(15, true);
            fSmall = mk(9, false);
            fFoot = mk(9.5f, false);

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
            RECT r{0, 0, (LONG)std::lround(layout::kWidth * scale), (LONG)std::lround(layout::kHeight * scale)};
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
            if (keyBusy[target]) note(b.text() + L" уже занято другой программой. Выберите другое.");
        }

        // ---------------------------------------------------------------- показ окна

        void showFromTray()
        {
            if (quitting) return;
            if (dlg)
            {
                SetForegroundWindow(dlg->wnd);
                return;
            }
            Mark(L"show");
            allowShow = true;
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else ShowWindow(hwnd, SW_SHOWNORMAL);
            // Размер «нормального» окна после странного запуска мог оказаться крошечным.
            RECT cr;
            GetClientRect(hwnd, &cr);
            if (cr.right < (LONG)std::lround(layout::kWidth * scale) || cr.bottom < (LONG)std::lround(layout::kHeight * scale)) layoutWindow();
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

        // Поверх всех окон только пока видна плёнка: иначе окно сидит в обычном порядке.
        void raiseAboveVeil()
        {
            if (!IsWindowVisible(hwnd)) return;
            bool veilUp = false;
            for (const Veil& v : veils)
                if (v.hwnd && IsWindowVisible(v.hwnd)) { veilUp = true; break; }
            SetWindowPos(hwnd, veilUp ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

        // ---------------------------------------------------------------- вопрос при закрытии окна

        enum class Answer { Cancel, Tray, Exit }; // порядок совпадает с кнопками слева направо

        struct Dialog
        {
            HWND wnd = nullptr;
            int hover = -1;
            int focus = 2;       // по умолчанию «Выйти»: человек нажал крестик
            int pressed = -1;
            bool ring = false;   // кольцо фокуса только после клавиатуры
            bool tracking = false;
            bool done = false;
            Answer answer = Answer::Cancel;
        };
        Dialog* dlg = nullptr;

        static int AskHit(const App& a, int px, int py)
        {
            int x = (int)std::lround(px / a.scale), y = (int)std::lround(py / a.scale);
            for (int i = 0; i < 3; i++)
                if (layout::In(layout::AskButton(i), x, y)) return i;
            return -1;
        }

        void drawAsk(G::Graphics& g, const Dialog& d)
        {
            using namespace layout;
            T(g, L"Выйти из Ночника?", *fValue, (float)kAskText.left, (float)kAskText.top, (float)(kAskText.right - kAskText.left), 30, cFg);
            T(g, L"Тёплый свет пропадёт, экран станет обычным.\nЧтобы свет остался, спрячьте Ночник в трей.", *fBase,
              (float)kAskText.left, (float)kAskText.top + 38, (float)(kAskText.right - kAskText.left),
              (float)(kAskText.bottom - kAskText.top - 38), cMuted);
            const wchar_t* names[3] = {L"Отмена", L"В трей", L"Выйти"};
            for (int i = 0; i < 3; i++)
            {
                const RECT r = AskButton(i);
                float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
                bool primary = i == 2, hot = d.hover == i, down = d.pressed == i && d.hover == i;
                G::Color fill = primary ? (hot ? cAccentHot : cAccent) : down ? cBtnDown : hot ? cBtnHot : cBtn;
                FillRound(g, fill, x, y, w, h, 6);
                if (!primary) StrokeRound(g, cBtnLine, 1, x, y, w, h, 6);
                T(g, names[i], *fBase, x, y, w, h, primary ? cBg : cFg, G::StringAlignmentCenter, G::StringAlignmentCenter);
                if (d.ring && d.focus == i) Ring(g, x, y, w, h, 6);
            }
        }

        static LRESULT CALLBACK AskProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            App* a = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (!a || !a->dlg) return DefWindowProcW(h, msg, wp, lp);
            Dialog& d = *a->dlg;
            auto finish = [&](Answer r) {
                d.answer = r;
                d.done = true;
            };
            auto move = [&](int dir) {
                d.focus = (d.focus + dir + 3) % 3;
                d.ring = true;
                InvalidateRect(h, nullptr, FALSE);
            };
            switch (msg)
            {
            case WM_CREATE:
            {
                BOOL on = TRUE;
                DwmSetWindowAttribute(h, 20, &on, sizeof on);
                COLORREF cap = Rgb(cBg), txt = Rgb(cFg);
                DwmSetWindowAttribute(h, 35, &cap, sizeof cap);
                DwmSetWindowAttribute(h, 36, &txt, sizeof txt);
                return 0;
            }
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(h, &ps);
                RECT cr;
                GetClientRect(h, &cr);
                a->buffered(hdc, cr.right, cr.bottom, [&](G::Graphics& g) { a->drawAsk(g, d); });
                EndPaint(h, &ps);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_MOUSEMOVE:
            {
                int hit = AskHit(*a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (!d.tracking)
                {
                    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                    d.tracking = TrackMouseEvent(&t) != FALSE;
                }
                if (hit != d.hover)
                {
                    d.hover = hit;
                    SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                d.tracking = false;
                d.hover = -1;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT)
                {
                    SetCursor(LoadCursorW(nullptr, d.hover >= 0 ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_LBUTTONDOWN:
                d.pressed = AskHit(*a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (d.pressed >= 0)
                {
                    d.focus = d.pressed;
                    d.ring = false;
                    SetCapture(h);
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_LBUTTONUP:
            {
                int hit = AskHit(*a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                int was = d.pressed;
                d.pressed = -1;
                if (GetCapture() == h) ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
                if (was >= 0 && hit == was) finish((Answer)hit);
                return 0;
            }
            case WM_KEYDOWN:
                switch (wp)
                {
                case VK_ESCAPE: finish(Answer::Cancel); break;
                case VK_RETURN:
                case VK_SPACE: finish((Answer)d.focus); break;
                case VK_TAB: move(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); break;
                case VK_LEFT: move(-1); break;
                case VK_RIGHT: move(1); break;
                }
                return 0;
            case WM_CLOSE:
                finish(Answer::Cancel);
                return 0;
            case WM_DESTROY:
                d.done = true;
                return 0;
            }
            return DefWindowProcW(h, msg, wp, lp);
        }

        // Тёмное окно с тремя кнопками. Пока оно открыто, главное окно недоступно.
        Answer askClose()
        {
            if (dlg)
            {
                SetForegroundWindow(dlg->wnd);
                return Answer::Cancel;
            }
            static const wchar_t kAskClass[] = L"NightAskWindow";
            static bool registered = false;
            if (!registered)
            {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof wc;
                wc.lpfnWndProc = &App::AskProc;
                wc.hInstance = inst;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
                wc.lpszClassName = kAskClass;
                registered = RegisterClassExW(&wc) != 0;
            }

            Dialog d;
            dlg = &d;
            const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_TOPMOST;
            RECT r{0, 0, (LONG)std::lround(layout::kAskWidth * scale), (LONG)std::lround(layout::kAskHeight * scale)};
            AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
            int w = r.right - r.left, h = r.bottom - r.top;
            RECT own;
            GetWindowRect(hwnd, &own);
            int x = own.left + ((own.right - own.left) - w) / 2, y = own.top + ((own.bottom - own.top) - h) / 2;
            d.wnd = CreateWindowExW(ex, kAskClass, L"Ночник", style, x, y, w, h, hwnd, nullptr, inst, this);
            if (!d.wnd)
            {
                dlg = nullptr;
                return Answer::Cancel;
            }
            EnableWindow(hwnd, FALSE);
            ShowWindow(d.wnd, SW_SHOW);
            SetForegroundWindow(d.wnd);

            MSG m;
            while (!d.done)
            {
                BOOL got = GetMessageW(&m, nullptr, 0, 0);
                if (got <= 0)
                {
                    if (got == 0) PostQuitMessage((int)m.wParam); // отдаём выход главному циклу
                    break;
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (IsWindow(hwnd)) EnableWindow(hwnd, TRUE); // до закрытия диалога: иначе фокус уйдёт в другое окно
            if (IsWindow(d.wnd)) DestroyWindow(d.wnd);
            dlg = nullptr;
            if (IsWindow(hwnd) && !quitting) SetForegroundWindow(hwnd);
            return d.answer;
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
            // Пока тянут ползунок, гамму пишем не чаще kApplyGap. Отпускание и таймер догоняют.
            ULONGLONG now = GetTickCount64();
            if (!(drag && now - lastApply < kApplyGap)) applyNow(true);
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
            ULONGLONG now = GetTickCount64();
            int d = effectiveDim();
            // Отказ драйвера не считаем успехом: повторяем сами, но не чаще kGammaRetry.
            // Сдвиг ползунка и смена экрана (forced) пробуют сразу.
            if (!forced && d == shownDim && (appliedOk || now < gammaRetryAt)) return;

            double strength, veil;
            Look::Split(d, strength, veil);
            bool ok = Gamma::Apply(strength);
            lastApply = now;

            shownDim = d;
            gammaOk = ok;
            appliedOk = ok;
            gammaRetryAt = ok ? 0 : now + kGammaRetry;

            if (d == 0)
            {
                hideVeils();
                return;
            }
            if (!ok)
            {
                Gamma::Identity();
                cover(d / 100.0 * Look::MaxVeil);
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
            // Ключ — только геометрия. Прозрачность place() меняет у уже живого окна,
            // иначе каждый шаг ползунка выше 70 уничтожает плёнку и на кадр гасит её.
            std::wstring key;
            for (const RECT& r : mons)
                key += std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right - r.left) + L"," +
                       std::to_wstring(r.bottom - r.top) + L";";
            if (key != veilKey || veils.size() != mons.size())
            {
                for (Veil& v : veils) v.destroy();
                veils.clear();
                veilKey = key;
                veils.resize(mons.size());
            }
            for (size_t i = 0; i < mons.size(); i++) veils[i].place(mons[i], opacity);
            raiseAboveVeil();
        }

        void hideVeils()
        {
            for (Veil& v : veils) v.destroy();
            veils.clear();
            veilKey.clear();
            raiseAboveVeil();
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
                text = cfg.keyToggle.empty() ? L"Обычный экран. Сдвиньте полосу, чтобы вернуть тёплый."
                                             : L"Обычный экран. " + cfg.keyToggle.text() + L" вернёт тёплый.";
            }
            else if (!gammaOk && dim > 0)
            {
                text = L"Видеодрайвер не принял цвет: экран только затемнён, без тёплого оттенка.";
                col = cWarn;
            }
            else if (dim == 0) text = L"Выключено. Экран обычный.";
            else if (dim == Look::Liked)
            {
                text = L"Рекомендуемый тёплый тон.";
                col = cAccent;
            }
            else if (dim > Look::Liked)
            {
                text = L"Темнее: тот же тёплый цвет, поверх него затемнение.";
                col = cAccent;
            }
            else
            {
                text = L"Светлее: тёплый оттенок слабее.";
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
            enum { Open = 1, Level, Lighter, Darker, Liked, Warm, Auto, Exit };
            // Название и сочетание в одну строку: сочетание за табуляцией прижимается к правому краю.
            auto item = [&](const wchar_t* name, const Bind& b) {
                return b.empty() ? std::wstring(name) : std::wstring(name) + L"\t" + b.text();
            };
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, Open, L"Открыть Ночник");
            SetMenuDefaultItem(m, Open, FALSE);
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (effectiveDim() > 0 ? MF_CHECKED : 0), Warm, item(L"Тёплый свет", cfg.keyToggle).c_str());
            AppendMenuW(m, MF_STRING | MF_GRAYED, Level, (L"Тусклость: " + std::to_wstring(dim)).c_str());
            AppendMenuW(m, MF_STRING, Lighter, item(L"Светлее", cfg.keyLighter).c_str());
            AppendMenuW(m, MF_STRING, Darker, item(L"Темнее", cfg.keyDarker).c_str());
            AppendMenuW(m, MF_STRING | (dim == Look::Liked ? MF_GRAYED : 0), Liked,
                        (L"Рекомендуемая (" + std::to_wstring(Look::Liked) + L")").c_str());
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
            case Warm:
                if (dim == 0) setDim(Look::Liked); // ползунок на нуле: просто включить не из чего
                else toggleHold();
                break;
            case Auto: setAutostart(!autoOn); break;
            case Exit: exitClean(); break;
            }
        }

        // ---------------------------------------------------------------- геометрия (размеры — в layout.h)

        int hitTest(int px, int py) const
        {
            int x = (int)std::lround(px / scale), y = (int)std::lround(py / scale);
            if (layout::In(layout::kSlider, x, y)) return HitSlider;
            if (layout::In(layout::kAutoCard, x, y)) return HitAuto;
            for (int i = 0; i < 3; i++)
                if (layout::In(layout::KeyButton(i), x, y)) return HitKey0 + i;
            return HitNone;
        }

        static double SliderX1() { return layout::kSlider.left + layout::kSliderPad; }
        static double SliderX2() { return layout::kSlider.right - layout::kSliderPad; }

        static int SliderValueAt(double x)
        {
            double t = std::clamp((x - SliderX1()) / (SliderX2() - SliderX1()), 0.0, 1.0);
            return (int)std::nearbyint(t * 100.0);
        }

        // ---------------------------------------------------------------- действия и фокус

        // Порядок Tab совпадает с номерами Hit: полоса, переключатель, три сочетания.
        static constexpr int kFocusCount = 5;

        void moveFocus(int dir)
        {
            if (focus < 0) focus = dir > 0 ? 0 : kFocusCount - 1;
            else focus = (focus + dir + kFocusCount) % kFocusCount;
            focusVisible = true;
            invalidate();
        }

        void activate(int hit)
        {
            if (hit == HitAuto) setAutostart(!autoOn);
            else if (hit >= HitKey0 && hit < HitKey0 + 3) beginCapture(hit - HitKey0);
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

        static void RoundPath(G::GraphicsPath& p, float x, float y, float w, float h, float r)
        {
            float d = r * 2;
            p.AddArc(x, y, d, d, 180, 90);
            p.AddArc(x + w - d, y, d, d, 270, 90);
            p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            p.AddArc(x, y + h - d, d, d, 90, 90);
            p.CloseFigure();
        }

        static void FillRound(G::Graphics& g, const G::Color& c, float x, float y, float w, float h, float r)
        {
            G::GraphicsPath p;
            RoundPath(p, x, y, w, h, r);
            G::SolidBrush b(c);
            g.FillPath(&b, &p);
        }

        // Рамка шириной width, целиком внутри прямоугольника.
        static void StrokeRound(G::Graphics& g, const G::Color& c, float width, float x, float y, float w, float h, float r)
        {
            G::GraphicsPath p;
            float k = width / 2;
            RoundPath(p, x + k, y + k, w - width, h - width, std::max(0.5f, r - k));
            G::Pen pen(c, width);
            g.DrawPath(&pen, &p);
        }

        static void Card(G::Graphics& g, const RECT& r, bool hot = false, bool frame = true, bool fill = true)
        {
            if (!fill && !hot) return;
            float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
            FillRound(g, hot ? cCardHot : cCard, x, y, w, h, 8);
            if (frame) StrokeRound(g, cCardLine, 1, x, y, w, h, 8);
        }

        // Кольцо клавиатурного фокуса снаружи прямоугольника.
        static void Ring(G::Graphics& g, float x, float y, float w, float h, float r)
        {
            StrokeRound(g, cRing, 2, x - 4, y - 4, w + 8, h + 8, r + 4);
        }

        static void Switch(G::Graphics& g, float x, float y, bool on, bool hot)
        {
            if (on)
            {
                FillRound(g, hot ? cAccentHot : cAccent, x, y, 40, 20, 10);
                G::SolidBrush knob(cBg);
                g.FillEllipse(&knob, x + 24, y + 4, 12.0f, 12.0f);
            }
            else
            {
                StrokeRound(g, hot ? cSwitchOffHot : cSwitchOff, 1.5f, x, y, 40, 20, 10);
                G::SolidBrush knob(hot ? cSwitchOffHot : cSwitchOff);
                g.FillEllipse(&knob, x + 5, y + 5, 10.0f, 10.0f);
            }
        }

        // Рисует в память и одним махом выкладывает в окно: без мерцания.
        template <class F>
        void buffered(HDC hdc, int w, int h, F&& fn)
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
                fn(g);
            }
            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }

        void paint(HDC hdc, int w, int h)
        {
            buffered(hdc, w, h, [&](G::Graphics& g) { draw(g); });
        }

        // Снимок окна в PNG (--render): проверка внешнего вида без экрана.
        bool renderPng(const std::wstring& path)
        {
            G::Bitmap bmp((INT)std::lround(layout::kWidth * scale), (INT)std::lround(layout::kHeight * scale), PixelFormat32bppRGB);
            {
                G::Graphics g(&bmp);
                g.SetSmoothingMode(G::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(G::TextRenderingHintClearTypeGridFit);
                g.ScaleTransform((G::REAL)scale, (G::REAL)scale);
                g.Clear(cBg);
                draw(g);
            }
            UINT n = 0, bytes = 0;
            G::GetImageEncodersSize(&n, &bytes);
            if (!n || !bytes) return false;
            std::vector<char> buf(bytes);
            auto* enc = reinterpret_cast<G::ImageCodecInfo*>(buf.data());
            G::GetImageEncoders(n, bytes, enc);
            for (UINT i = 0; i < n; i++)
                if (wcscmp(enc[i].MimeType, L"image/png") == 0) return bmp.Save(path.c_str(), &enc[i].Clsid, nullptr) == G::Ok;
            return false;
        }

        void draw(G::Graphics& g)
        {
            using namespace layout;
            const auto Near = G::StringAlignmentNear;
            const auto Center = G::StringAlignmentCenter;
            const auto Far = G::StringAlignmentFar;
            auto focused = [&](int hit) { return focusVisible && focus == hit; };

            // Название уже в заголовке окна.
            T(g, L"Тёплый свет для экрана по вечерам", *fSub, 20, 18, 380, 22, cMuted);

            // Тусклость.
            Card(g, kLevelCard);
            T(g, L"Тусклость", *fBase, 32, 78, 180, 24, cFg);
            T(g, std::to_wstring(dim), *fValue, 200, 72, 188, 30, cAccent, Far, Center);
            {
                float x1 = (float)SliderX1(), x2 = (float)SliderX2();
                float cy = kSlider.top + (kSlider.bottom - kSlider.top) / 2.0f;
                float tx = x1 + (x2 - x1) * (bar / 100.0f);
                FillRound(g, cTrack, x1, cy - 2, x2 - x1, 4, 2);
                if (tx - x1 >= 1.0f) FillRound(g, cAccent, x1, cy - 2, tx - x1, 4, 2);

                float mx = x1 + (x2 - x1) * (Look::Liked / 100.0f);
                G::Pen mark(cMuted, 1.5f);
                mark.SetStartCap(G::LineCapRound);
                mark.SetEndCap(G::LineCapRound);
                g.DrawLine(&mark, mx, cy - 17, mx, cy - 12);

                bool hot = drag || hover == HitSlider;
                G::SolidBrush glow(G::Color(hot ? 96 : 52, cAccent.GetR(), cAccent.GetG(), cAccent.GetB()));
                g.FillEllipse(&glow, tx - 14, cy - 14, 28.0f, 28.0f);
                G::SolidBrush rim(cThumb);
                g.FillEllipse(&rim, tx - 9, cy - 9, 18.0f, 18.0f);
                float ri = drag ? 4.0f : hot ? 6.5f : 5.5f;
                G::SolidBrush core(hot ? cAccentHot : cAccent);
                g.FillEllipse(&core, tx - ri, cy - ri, ri * 2, ri * 2);
                if (focused(HitSlider))
                {
                    G::Pen ring(cRing, 2.0f);
                    g.DrawEllipse(&ring, tx - 13, cy - 13, 26.0f, 26.0f);
                }
            }
            T(g, L"светлее", *fSmall, 32, 144, 160, 18, cHint);
            T(g, L"темнее", *fSmall, 228, 144, 160, 18, cHint, Far);
            T(g, statusText, *fSub, 32, 168, 356, 36, statusColor);

            // Автозапуск — строка без карточки. Заливка только под курсором.
            {
                Card(g, kAutoCard, hover == HitAuto, false, false);
                float h = (float)(kAutoCard.bottom - kAutoCard.top);
                T(g, L"Запускать вместе с Windows", *fBase, 32, (float)kAutoCard.top, 300, h, cFg, Near, Center);
                Switch(g, kAutoCard.right - 16.0f - 40, kAutoCard.top + (h - 20) / 2, autoOn, hover == HitAuto);
                if (focused(HitAuto))
                    Ring(g, (float)kAutoCard.left, (float)kAutoCard.top, (float)(kAutoCard.right - kAutoCard.left), h, 8);
            }

            // Горячие клавиши.
            T(g, L"Горячие клавиши", *fBold, 20, 272, 380, 20, cFg);
            Card(g, kKeysCard, false, false);
            for (int i = 0; i < 3; i++)
            {
                int rowTop = kKeysCard.top + i * kKeyRow;
                if (i > 0)
                {
                    G::Pen line(cCardLine, 1.0f);
                    g.DrawLine(&line, 32.0f, (float)rowTop, (float)(kKeysCard.right - 16), (float)rowTop);
                }
                T(g, kKeyTitles[i], *fBase, 32, (float)rowTop, 190, (float)kKeyRow, cFg, Near, Center);

                std::wstring text;
                G::Color fg = cFg;
                if (capture == i)
                {
                    text = L"Нажмите сочетание…";
                    fg = cAccent;
                }
                else
                {
                    const Bind& b = keyOf(i);
                    text = b.text();
                    if (b.empty()) fg = cHint;
                    else if (keyBusy[i])
                    {
                        text += L" — занято";
                        fg = cWarn;
                    }
                }
                RECT r = KeyButton(i);
                float bw = (float)(r.right - r.left), bh = (float)(r.bottom - r.top);
                bool hotKey = hover == HitKey0 + i;
                bool isDown = pressed == HitKey0 + i && hover == pressed;
                G::Color fillc = cCard;
                G::Color linec = cCardLine;
                if (capture == i)
                {
                    fillc = G::Color(255, 54, 46, 38);
                    linec = cAccent;
                }
                else if (isDown)
                {
                    fillc = cBtnDown;
                    linec = cBtnLine;
                }
                else if (hotKey)
                {
                    fillc = cCardHot;
                    linec = cBtnLine;
                }
                FillRound(g, fillc, (float)r.left, (float)r.top, bw, bh, 6);
                StrokeRound(g, linec, 1, (float)r.left, (float)r.top, bw, bh, 6);
                T(g, text, *fBase, (float)r.left, (float)r.top, bw, bh, fg, Center, Center);
                if (focused(HitKey0 + i)) Ring(g, (float)r.left, (float)r.top, bw, bh, 6);
            }

            bool shown = noteActive();
            T(g, shown ? keyNote : std::wstring(kKeyHelp), *fSmall, 20, 410, 380, 36, shown ? cWarn : cHint);

            // Низ окна.
            T(g, L"Свернуть прячет в трей, крестик закрывает Ночник.", *fFoot, 20, 452, 380, 22, cHint, Near, Center);
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
                    Answer a = askClose();
                    if (a == Answer::Exit) exitClean();
                    else if (a == Answer::Tray) hideToTray();
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
                    case VK_LEFT: case VK_DOWN: nudge(-step); return 0;
                    case VK_RIGHT: case VK_UP: nudge(step); return 0;
                    case VK_PRIOR: nudge(10); return 0;
                    case VK_NEXT: nudge(-10); return 0;
                    case VK_TAB: moveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0;
                    case VK_SPACE: case VK_RETURN:
                        if (!(lp & (1 << 30))) activate(focus); // повтор при удержании не считаем
                        return 0;
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
                focus = hit;
                focusVisible = false;
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
                bool wasDrag = drag;
                pressed = HitNone;
                drag = false;
                ReleaseCapture();
                // Последние шаги ползунка могли не дойти до гаммы из-за паузы kApplyGap.
                if (wasDrag) applyNow(true);
                if (hit != HitNone && hit == was)
                {
                    activate(hit);
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

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
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

    std::wstring renderPath = ArgAfter(argc, argv, L"--render"); // снимок окна в PNG: --render файл.png [--dim N] [--focus N] [--hover N] [--capture N]
    bool checkUi = HasArg(argc, argv, L"--check-ui") || !renderPath.empty();
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
        // Не удалось создать замок — работаем без защиты от второй копии, чем не запускаться вовсе.
        DWORD wait = mutex ? WaitForSingleObject(mutex, 0) : WAIT_OBJECT_0;
        if (mutex && wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
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
        else if (!renderPath.empty())
        {
            auto num = [&](const wchar_t* name, int fallback) {
                int v;
                return ParseInt(ArgAfter(argc, argv, name), v) ? v : fallback;
            };
            app.dim = app.bar = std::clamp(num(L"--dim", Look::Liked), 0, 100);
            app.focus = num(L"--focus", HitNone);
            app.focusVisible = app.focus != HitNone;
            app.hover = num(L"--hover", HitNone);
            app.capture = num(L"--capture", -1);
            app.refreshTexts();
            code = app.renderPng(renderPath) ? 0 : 1;
            DestroyWindow(app.hwnd);
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