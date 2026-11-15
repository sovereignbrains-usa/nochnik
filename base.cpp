#include "base.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace night
{
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
        std::wstring path = dir + L"\\log.txt";
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        {
            unsigned long long size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
            if (size > kLogLimit) MoveFileExW(path.c_str(), (dir + L"\\log.old.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        WriteFileUtf8(path, ToUtf8(std::wstring(stamp) + line + L"\r\n"), true);
    }
}