#include "display.h"

#include <mutex>
#include <utility>

namespace night
{
    // ------------------------------------------------------------------ вид

    namespace Look
    {
        void Split(int dim, double& strength, double& veil)
        {
            dim = std::clamp(dim, 0, 100);
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

    // ------------------------------------------------------------------ гамма

    namespace Gamma
    {
        namespace
        {
            std::mutex gate;

            struct Pass
            {
                const unsigned short* ramp;
                int monitors = 0;
                int accepted = 0;
            };

            BOOL CALLBACK EnumProc(HMONITOR mon, HDC, LPRECT, LPARAM data)
            {
                auto* st = reinterpret_cast<Pass*>(data);
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof mi;
                if (!GetMonitorInfoW(mon, &mi)) return TRUE;
                st->monitors++;
                HDC dc = CreateDCW(mi.szDevice, nullptr, nullptr, nullptr);
                if (!dc) return TRUE;
                if (SetDeviceGammaRamp(dc, const_cast<unsigned short*>(st->ramp))) st->accepted++;
                DeleteDC(dc);
                return TRUE;
            }

            bool ApplyRamp(double strength)
            {
                unsigned short ramp[768];
                Ramp(strength, ramp);
                Pass st{ramp};
                EnumDisplayMonitors(nullptr, nullptr, EnumProc, reinterpret_cast<LPARAM>(&st));
                return st.monitors > 0 && st.monitors == st.accepted;
            }
        }

        void Fill(unsigned short* ramp, double mul)
        {
            mul = std::clamp(mul, 0.0, 1.0);
            int prev = 0;
            for (int i = 0; i < 256; i++)
            {
                int v = (int)std::nearbyint(i * 257.0 * mul);
                v = std::clamp(v, prev, 65535);
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

        bool Apply(double strength)
        {
            std::lock_guard<std::mutex> lock(gate);
            return ApplyRamp(strength);
        }

        void Identity() { Apply(0); }

        void IdentityUnlocked() { ApplyRamp(0); }
    }

    // ------------------------------------------------------------------ плёнка

    const wchar_t kVeilClass[] = L"NightVeil";

    void Veil::place(const RECT& r, double opacity)
    {
        if (opacity < 0.02)
        {
            hide();
            return;
        }
        opacity = std::min(opacity, Look::MaxVeil);
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

    void Veil::hide()
    {
        if (hwnd) ShowWindow(hwnd, SW_HIDE);
    }

    void Veil::destroy()
    {
        if (hwnd) DestroyWindow(hwnd);
        hwnd = nullptr;
    }
}
