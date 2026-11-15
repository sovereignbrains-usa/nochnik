#pragma once
// Всё, что меняет картинку на экране: цвет (гамма-рампа) и затемняющая плёнка.

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace night
{
    // Тёплый свет зафиксирован (02.11.2026): R 0.72, G 0.60, B 0.50. Тусклость до Liked ослабляет тот же цвет.
    // Дальше драйвер не принимает канал ниже ~0.50, поэтому доп. темноту даёт прозрачная чёрная плёнка.
    namespace Look
    {
        constexpr double Red = 0.72;
        constexpr double Green = 0.60;
        constexpr double Blue = 0.50;
        constexpr int Liked = 70;
        constexpr double MaxVeil = 0.75;

        // Тусклость 0..100 → сила цвета 0..1 и непрозрачность плёнки 0..MaxVeil.
        void Split(int dim, double& strength, double& veil);
        // Множитель канала: 1 (нет цвета) … night (полный).
        double Channel(double strength, double night);
    }

    namespace Gamma
    {
        void Fill(unsigned short* ramp, double mul);
        void Ramp(double strength, unsigned short (&ramp)[768]);
        // true — гамма принята на всех экранах.
        bool Apply(double strength);
        void Identity();
        // То же без блокировки: из обработчика падения, где замок мог остаться захваченным.
        void IdentityUnlocked();
    }

    extern const wchar_t kVeilClass[];

    // Прозрачное чёрное окно поверх монитора, клики проходят сквозь него.
    struct Veil
    {
        HWND hwnd = nullptr;

        void place(const RECT& r, double opacity);
        void hide();
        void destroy();
    };
}
