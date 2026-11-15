#pragma once
// Сочетания клавиш и файл настроек.

#include <windows.h>

#include <string>

#include "base.h"
#include "display.h"

namespace night
{
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
            unsigned long m = 0, v = 0;
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

    struct Config
    {
        int dim = Look::Liked;
        bool autostart = false;
        Bind keyLighter{Bind::Ctrl | Bind::Alt, VK_UP};
        Bind keyDarker{Bind::Ctrl | Bind::Alt, VK_DOWN};
        Bind keyToggle{Bind::Ctrl | Bind::Alt, 'N'};

        static std::wstring filePath(); // %AppData%\Night\config.txt

        // Разбор и сборка текста: чужие и битые строки пропускаются, значения зажимаются в границы.
        static Config parse(const std::string& raw);
        std::string serialize() const;

        static Config load(const std::wstring& path);
        static Config load() { return load(filePath()); }
        bool save(const std::wstring& path) const;
        bool save() const;
    };

    // Автозапуск вне пакета: значение Night в HKCU\...\Run.
    void SetRunKey(bool on);
}