#pragma once
// Геометрия окна в «96-dpi единицах»: окно рисуется в них и растягивается под масштаб экрана.
// Лежит отдельно, чтобы самопроверка могла убедиться, что ничего не налезает друг на друга.

#include <windows.h>

namespace night::layout
{
    constexpr int kWidth = 420;
    constexpr int kHeight = 488;

    constexpr RECT kLevelCard{16, 64, 404, 206};   // тусклость: значение, полоса, пояснение
    constexpr RECT kSlider{16, 98, 404, 140};      // зона полосы (нажимается вся)
    constexpr int kSliderPad = 20;                 // отступ дорожки от краёв зоны
    constexpr RECT kAutoCard{16, 216, 404, 260};   // строка с переключателем, нажимается вся
    constexpr RECT kKeysCard{16, 296, 404, 404};   // три строки сочетаний
    constexpr int kKeyRow = 36;
    constexpr RECT kExit{316, 448, 404, 476};

    constexpr RECT KeyButton(int i)
    {
        return RECT{232, kKeysCard.top + i * kKeyRow + 4, 388, kKeysCard.top + i * kKeyRow + 32};
    }

    constexpr bool In(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

    constexpr bool Inside(const RECT& inner, const RECT& outer)
    {
        return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
    }

    constexpr bool Overlap(const RECT& a, const RECT& b)
    {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    }
}
