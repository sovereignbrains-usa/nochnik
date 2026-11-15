#pragma once
// Общие мелочи: строки, файлы, журнал. Ни от чего в программе не зависит.

#include <string>

namespace night
{
    std::wstring FromUtf8(const std::string& s);
    std::string ToUtf8(const std::wstring& w);
    std::wstring Trim(const std::wstring& s);

    // Целое число из всей строки (пробелы по краям допустимы). false — там не число.
    bool ParseUInt(const std::wstring& s, unsigned long& out);
    bool ParseInt(const std::wstring& s, int& out);

    std::wstring AppDataDir();   // %AppData%\Night
    std::wstring ExePath();

    bool WriteFileUtf8(const std::wstring& path, const std::string& bytes, bool append);
    bool ReadFileUtf8(const std::wstring& path, std::string& out); // не читает файлы больше 1 МБ

    // Строка в %AppData%\Night\log.txt с временем. Файл не растёт бесконечно: после kLogLimit байт старый уходит в log.old.txt.
    void Mark(const std::wstring& line);
    constexpr unsigned long long kLogLimit = 256 * 1024;
}
