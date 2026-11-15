#pragma once
// Самопроверки без окна: `Night.exe --check [файл]`. Пишут отчёт в файл, код выхода 0 — всё хорошо.

#include <string>

namespace night
{
    int Check(const std::wstring& outPath);
}
