#include "config.h"

#include <algorithm>

namespace night
{
    std::wstring Config::filePath() { return AppDataDir() + L"\\config.txt"; }

    Config Config::parse(const std::string& rawBytes)
    {
        Config c;
        std::string raw = rawBytes;
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

    std::string Config::serialize() const
    {
        std::wstring s;
        s += L"dim=" + std::to_wstring(dim) + L"\n";
        s += std::wstring(L"autostart=") + (autostart ? L"1" : L"0") + L"\n";
        s += L"key_lighter=" + keyLighter.save() + L"\n";
        s += L"key_darker=" + keyDarker.save() + L"\n";
        s += L"key_toggle=" + keyToggle.save() + L"\n";
        return ToUtf8(s);
    }

    Config Config::load(const std::wstring& path)
    {
        std::string raw;
        if (!ReadFileUtf8(path, raw)) return Config();
        return parse(raw);
    }

    bool Config::save(const std::wstring& path) const
    {
        // Сначала во временный файл и подмена: оборванная запись не испортит настройки.
        std::wstring tmp = path + L".tmp";
        if (!WriteFileUtf8(tmp, serialize(), false)) return false;
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    bool Config::save() const
    {
        CreateDirectoryW(AppDataDir().c_str(), nullptr);
        return save(filePath());
    }

    // Автозапуск вне пакета (обычный установщик): значение в HKCU\...\Run. В пакете — StartupTask, см. startup.cpp.
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
}
