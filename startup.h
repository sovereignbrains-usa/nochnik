#pragma once
// Автозапуск из MSIX-пакета. В пакете запись в HKCU\...\Run уходит в частный куст и не работает,
// поэтому там используется StartupTask из манифеста. Это единственное место, где нужен C++/WinRT.

namespace Startup
{
    enum class State { Enabled, Disabled, DisabledByUser, DisabledByPolicy, Error };

    // Работает ли процесс внутри MSIX-пакета.
    bool Packaged();

    // Все три вызова блокируют: звать из рабочего потока.
    State Query();
    State Set(bool on);

    // Запущен ли процесс системой как задача автозапуска (тогда окно не открываем).
    bool LaunchedByStartupTask();
    int ActivationKind(); // -1 — не удалось узнать (вне пакета или ошибка)
}
