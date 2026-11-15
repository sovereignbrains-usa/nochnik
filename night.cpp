// Ночник. Тёплый свет зафиксирован (02.11.2026): R 0.72, G 0.60, B 0.50.
// Тусклость до этой точки ослабляет тот же цвет. Дальше драйвер не принимает
// канал ниже ~0.50, поэтому доп. темноту даёт прозрачная чёрная плёнка.
// Клики сквозь плёнку проходят. Подсветку монитора не трогает. Без админа.
// На выходе гамма обычная.
//
// Чистый Win32 + GDI+, без рантаймов (/MT). Интерфейс рисуется в одном окне.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

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

namespace
{
    // ------------------------------------------------------------------ утилиты

    std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    std::wstring Trim(const std::wstring& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && iswspace(s[a])) a++;
        while (b > a && iswspace(s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    bool ParseUInt(const std::wstring& s, unsigned long& out)
    {
        std::wstring t = Trim(s);
        if (t.empty() || !iswdigit(t[0])) return false;
        wchar_t* end = nullptr;
        out = wcstoul(t.c_str(), &end, 10);
        return end && *end == L'\0';
    }

    bool ParseInt(const std::wstring& s, int& out)
    {
        std::wstring t = Trim(s);
        if (t.empty()) return false;
        wchar_t* end = nullptr;
        long v = wcstol(t.c_str(), &end, 10);
        if (!end || *end != L'\0') return false;
        out = (int)v;
        return true;
    }

    std::wstring AppDataDir()
    {
        PWSTR p = nullptr;
        std::wstring r;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) r = p;
        CoTaskMemFree(p);
        return r + L"\\Night";
    }

    std::wstring ExePath()
    {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
        return std::wstring(buf, n);
    }

    bool WriteFileUtf8(const std::wstring& path, const std::string& bytes, bool append)
    {
        HANDLE f = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD w = 0;
        bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &w, nullptr) && w == bytes.size();
        CloseHandle(f);
        return ok;
    }

    bool ReadFileUtf8(const std::wstring& path, std::string& out)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        if (size.QuadPart > 1 << 20) { CloseHandle(f); return false; }
        out.assign((size_t)size.QuadPart, '\0');
        DWORD r = 0;
        bool ok = ReadFile(f, out.data(), (DWORD)out.size(), &r, nullptr);
        CloseHandle(f);
        out.resize(r);
        return ok;
    }

    void Mark(const std::wstring& line)
    {
        std::wstring dir = AppDataDir();
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[16];
        swprintf_s(stamp, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
        WriteFileUtf8(dir + L"\\log.txt", ToUtf8(std::wstring(stamp) + line + L"\r\n"), true);
    }

    // ------------------------------------------------------------------ вид

    namespace Look
    {
        constexpr double Red = 0.72;
        constexpr double Green = 0.60;
        constexpr double Blue = 0.50;
        constexpr int Liked = 70;
        constexpr double MaxVeil = 0.75;

        void Split(int dim, double& strength, double& veil)
        {
            if (dim < 0) dim = 0;
            if (dim > 100) dim = 100;
            if (dim <= Liked)
            {
                strength = dim / (double)Liked;
                veil = 0;
            }
            else
            {
                strength = 1;
                veil = (dim - Liked) / (double)(100 - Liked) * MaxVeil;
            }
        }

        double Channel(double strength, double night)
        {
            if (strength >= 1) return night;
            if (strength <= 0) return 1;
            return 1.0 - strength * (1.0 - night);
        }
    }

    // ------------------------------------------------------------------ сочетание клавиш

    // Модификаторы MOD_* и виртуальная клавиша. Vk 0 — не назначено.
    struct Bind
    {
        static constexpr UINT Alt = 0x0001, Ctrl = 0x0002, Shift = 0x0004, Win = 0x0008;

        UINT mods = 0;
        UINT vk = 0;

        Bind() = default;
        Bind(UINT m, UINT v) : mods(m & 0x000F), vk(v & 0xFF) {}

        bool empty() const { return vk == 0; }
        bool same(const Bind& o) const { return !empty() && mods == o.mods && vk == o.vk; }

        std::wstring save() const { return std::to_wstring(mods) + L":" + std::to_wstring(vk); }

        static bool parse(const std::wstring& s, Bind& out)
        {
            size_t colon = s.find(L':');
            if (colon == std::wstring::npos || s.find(L':', colon + 1) != std::wstring::npos) return false;
            unsigned long m, v;
            if (!ParseUInt(s.substr(0, colon), m) || !ParseUInt(s.substr(colon + 1), v)) return false;
            out = Bind((UINT)m, (UINT)v);
            return true;
        }

        // false — нажат только модификатор, ждём дальше.
        static bool fromVk(UINT vk, UINT mods, Bind& out)
        {
            switch (vk)
            {
            case 0: case VK_SHIFT: case VK_CONTROL: case VK_MENU:
            case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
            case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
                return false;
            }
            out = Bind(mods, vk);
            return true;
        }

        static UINT currentMods()
        {
            UINT m = 0;
            if (GetKeyState(VK_CONTROL) < 0) m |= Ctrl;
            if (GetKeyState(VK_MENU) < 0) m |= Alt;
            if (GetKeyState(VK_SHIFT) < 0) m |= Shift;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= Win;
            return m;
        }

        // Без Ctrl/Alt/Win буква или цифра будет перехватываться при наборе текста.
        bool safeAlone() const
        {
            if ((mods & (Ctrl | Alt | Win)) != 0) return true;
            if (vk >= 0x70 && vk <= 0x87) return true;
            return vk == 0x13 || vk == 0x91;
        }

        std::wstring text() const
        {
            if (empty()) return L"не назначено";
            std::wstring s;
            if (mods & Ctrl) s += L"Ctrl+";
            if (mods & Alt) s += L"Alt+";
            if (mods & Shift) s += L"Shift+";
            if (mods & Win) s += L"Win+";
            return s + keyName(vk);
        }

        static std::wstring keyName(UINT vk)
        {
            if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A)) return std::wstring(1, (wchar_t)vk);
            if (vk >= 0x60 && vk <= 0x69) return L"Num " + std::to_wstring(vk - 0x60);
            if (vk >= 0x70 && vk <= 0x87) return L"F" + std::to_wstring(vk - 0x70 + 1);
            switch (vk)
            {
            case VK_UP: return L"↑";
            case VK_DOWN: return L"↓";
            case VK_LEFT: return L"←";
            case VK_RIGHT: return L"→";
            case VK_PRIOR: return L"PgUp";
            case VK_NEXT: return L"PgDn";
            case VK_HOME: return L"Home";
            case VK_END: return L"End";
            case VK_INSERT: return L"Ins";
            case VK_DELETE: return L"Del";
            case VK_SPACE: return L"Пробел";
            case VK_RETURN: return L"Enter";
            case VK_BACK: return L"Backspace";
            case VK_TAB: return L"Tab";
            case VK_OEM_PLUS: return L"=";
            case VK_OEM_MINUS: return L"-";
            case VK_OEM_COMMA: return L",";
            case VK_OEM_PERIOD: return L".";
            case VK_ADD: return L"Num +";
            case VK_SUBTRACT: return L"Num -";
            case VK_MULTIPLY: return L"Num *";
            case VK_DIVIDE: return L"Num /";
            case VK_PAUSE: return L"Pause";
            case VK_SCROLL: return L"ScrollLock";
            }
            UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            wchar_t buf[64] = {};
            if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, (int)std::size(buf)) > 0) return buf;
            return L"Key " + std::to_wstring(vk);
        }
    };

    // ------------------------------------------------------------------ настройки

    struct Config
    {
        int dim = Look::Liked;
        bool autostart = false;
        Bind keyLighter{Bind::Ctrl | Bind::Alt, VK_UP};
        Bind keyDarker{Bind::Ctrl | Bind::Alt, VK_DOWN};
        Bind keyToggle{Bind::Ctrl | Bind::Alt, 'N'};

        static std::wstring filePath() { return AppDataDir() + L"\\config.txt"; }

        static Config load()
        {
            Config c;
            std::string raw;
            if (!ReadFileUtf8(filePath(), raw)) return c;
            if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
                raw.erase(0, 3);
            std::wstring text = FromUtf8(raw);
            size_t pos = 0;
            while (pos <= text.size())
            {
                size_t nl = text.find(L'\n', pos);
                if (nl == std::wstring::npos) nl = text.size();
                std::wstring line = text.substr(pos, nl - pos);
                pos = nl + 1;
                size_t eq = line.find(L'=');
                if (eq == std::wstring::npos || eq < 1) continue;
                std::wstring k = Trim(line.substr(0, eq));
                std::wstring v = Trim(line.substr(eq + 1));
                int n;
                Bind b;
                if (k == L"dim") { if (ParseInt(v, n)) c.dim = std::clamp(n, 0, 100); }
                else if (k == L"autostart") c.autostart = v == L"1";
                else if (k == L"key_lighter") { if (Bind::parse(v, b)) c.keyLighter = b; }
                else if (k == L"key_darker") { if (Bind::parse(v, b)) c.keyDarker = b; }
                else if (k == L"key_toggle") { if (Bind::parse(v, b)) c.keyToggle = b; }
            }
            return c;
        }

        bool save() const
        {
            std::wstring path = filePath();
            CreateDirectoryW(AppDataDir().c_str(), nullptr);
            std::wstring s;
            s += L"dim=" + std::to_wstring(dim) + L"\n";
            s += std::wstring(L"autostart=") + (autostart ? L"1" : L"0") + L"\n";
            s += L"key_lighter=" + keyLighter.save() + L"\n";
            s += L"key_darker=" + keyDarker.save() + L"\n";
            s += L"key_toggle=" + keyToggle.save() + L"\n";
            std::wstring tmp = path + L".tmp";
            if (!WriteFileUtf8(tmp, ToUtf8(s), false)) return false;
            if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                DeleteFileW(tmp.c_str());
                return false;
            }
            return true;
        }
    };

    // Автозапуск вне пакета (отладочная сборка): значение в HKCU\...\Run. В пакете — StartupTask, см. startup.cpp.
    void SetRunKey(bool on)
    {
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return;
        if (on)
        {
            std::wstring cmd = L"\"" + ExePath() + L"\" --background";
            RegSetValueExW(key, L"Night", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        }
        else RegDeleteValueW(key, L"Night");
        RegCloseKey(key);
    }

    // ------------------------------------------------------------------ гамма

    namespace Gamma
    {
        std::mutex gate;

        void Fill(unsigned short* ramp, double mul)
        {
            mul = std::clamp(mul, 0.0, 1.0);
            int prev = 0;
            for (int i = 0; i < 256; i++)
            {
                int v = (int)std::nearbyint(i * 257.0 * mul);
                if (v < prev) v = prev;
                if (v > 65535) v = 65535;
                ramp[i] = (unsigned short)v;
                prev = v;
            }
        }

        void Ramp(double strength, unsigned short (&ramp)[768])
        {
            Fill(ramp, Look::Channel(strength, Look::Red));
            Fill(ramp + 256, Look::Channel(strength, Look::Green));
            Fill(ramp + 512, Look::Channel(strength, Look::Blue));
        }

        BOOL CALLBACK EnumProc(HMONITOR mon, HDC, LPRECT, LPARAM data)
        {
            auto* st = (std::pair<unsigned short*, std::pair<int, int>>*)data;
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            if (!GetMonitorInfoW(mon, &mi)) return TRUE;
            st->second.first++;
            HDC dc = CreateDCW(mi.szDevice, nullptr, nullptr, nullptr);
            if (!dc) return TRUE;
            if (SetDeviceGammaRamp(dc, st->first)) st->second.second++;
            DeleteDC(dc);
            return TRUE;
        }

        // true — гамма принята на всех экранах.
        bool Apply(double strength)
        {
            std::lock_guard<std::mutex> lock(gate);
            unsigned short ramp[768];
            Ramp(strength, ramp);
            std::pair<unsigned short*, std::pair<int, int>> st{ramp, {0, 0}};
            EnumDisplayMonitors(nullptr, nullptr, EnumProc, (LPARAM)&st);
            return st.second.first > 0 && st.second.first == st.second.second;
        }

        void Identity() { Apply(0); }
    }

    // ------------------------------------------------------------------ плёнка

    const wchar_t kVeilClass[] = L"NightVeil";

    struct Veil
    {
        HWND hwnd = nullptr;

        void place(const RECT& r, double opacity)
        {
            if (opacity < 0.02)
            {
                hide();
                return;
            }
            if (opacity > Look::MaxVeil) opacity = Look::MaxVeil;
            if (!hwnd)
            {
                hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                       kVeilClass, L"", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr,
                                       nullptr, GetModuleHandleW(nullptr), nullptr);
                if (!hwnd) return;
            }
            SetLayeredWindowAttributes(hwnd, 0, (BYTE)std::lround(opacity * 255.0), LWA_ALPHA);
            SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        void hide()
        {
            if (hwnd) ShowWindow(hwnd, SW_HIDE);
        }

        void destroy()
        {
            if (hwnd) DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    };

    // ------------------------------------------------------------------ окно

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

    // ------------------------------------------------------------------ самопроверки (--check)

    struct Report
    {
        std::string text;
        int bad = 0;

        void line(bool ok, const char* name, const std::string& detail = "")
        {
            text += ok ? "ok   " : "FAIL ";
            text += name;
            if (!detail.empty()) text += " " + detail;
            text += "\n";
            if (!ok) bad++;
        }

        void eq(const char* name, double got, double expect)
        {
            char buf[96];
            snprintf(buf, sizeof buf, "got=%.3f expect=%.3f", got, expect);
            line(std::fabs(got - expect) < 0.001, name, buf);
        }

        void one(const char* name, int got, int expect)
        {
            char buf[96];
            snprintf(buf, sizeof buf, "got=%d expect=%d", got, expect);
            line(got == expect, name, buf);
        }

        void flag(const char* name, bool got, bool expect)
        {
            char buf[64];
            snprintf(buf, sizeof buf, "got=%d expect=%d", got ? 1 : 0, expect ? 1 : 0);
            line(got == expect, name, buf);
        }
    };

    int Check(const std::wstring& outPath)
    {
        Report r;
        double s, v;
        Look::Split(0, s, v);
        r.eq("off-s", s, 0);
        r.eq("off-v", v, 0);
        Look::Split(Look::Liked, s, v);
        r.eq("liked-s", s, 1);
        r.eq("liked-v", v, 0);
        Look::Split(35, s, v);
        r.eq("mid-s", s, 0.5);
        r.eq("mid-v", v, 0);
        Look::Split(85, s, v);
        r.eq("deep-s", s, 1);
        r.eq("deep-v", v, Look::MaxVeil / 2);
        Look::Split(100, s, v);
        r.eq("max-s", s, 1);
        r.eq("max-v", v, Look::MaxVeil);
        r.eq("ch-half", Look::Channel(0.5, Look::Red), 0.86);
        r.eq("ch-full", Look::Channel(1, Look::Red), Look::Red);

        Config d;
        r.flag("key-up-text", d.keyLighter.text() == L"Ctrl+Alt+↑", true);
        r.flag("key-n-text", d.keyToggle.text() == L"Ctrl+Alt+N", true);
        Bind rt;
        r.flag("key-round", Bind::parse(d.keyDarker.save(), rt) && rt.same(d.keyDarker), true);
        Bind bad;
        r.flag("key-bad", !Bind::parse(L"x:1", bad), true);
        Bind empty(1, 1);
        r.flag("key-empty", Bind::parse(L"0:0", empty) && empty.empty(), true);
        Bind from;
        r.flag("key-from", Bind::fromVk(VK_F5, Bind::Ctrl | Bind::Shift, from) && from.text() == L"Ctrl+Shift+F5", true);
        Bind modOnly;
        r.flag("key-mod-only", !Bind::fromVk(VK_CONTROL, Bind::Ctrl, modOnly), true);
        r.flag("key-letter-alone", Bind(0, 0x41).safeAlone(), false);
        r.flag("key-f9-alone", Bind(0, 0x78).safeAlone(), true);

        unsigned short liked[768], plain[768], half[768], again[768];
        Gamma::Ramp(1, liked);
        Gamma::Ramp(0, plain);
        Gamma::Ramp(0.5, half);
        r.one("r70", liked[128], 23685);
        r.one("g70", liked[384], 19738);
        r.one("b70", liked[640], 16448);
        r.one("id", plain[128], 32896);
        r.one("idg", plain[384], 32896);
        r.one("idb", plain[640], 32896);
        r.one("half", half[128], 28291);

        Gamma::Ramp(1, again);
        r.line(memcmp(liked, again, sizeof liked) == 0, "stable");
        bool mono = true;
        for (int ch = 0; ch < 3; ch++)
            for (int i = 1; i < 256; i++)
                if (liked[ch * 256 + i] < liked[ch * 256 + i - 1]) mono = false;
        r.line(mono, "mono");

        char tail[32];
        snprintf(tail, sizeof tail, "bad=%d\n", r.bad);
        r.text += tail;
        WriteFileUtf8(outPath, r.text, false);
        return r.bad == 0 ? 0 : 1;
    }

    // ------------------------------------------------------------------ запуск

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
        Gamma::Identity();
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
