#include "selfcheck.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "base.h"
#include "config.h"
#include "display.h"
#include "layout.h"

namespace night
{
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

        // Настройки: круг «собрать — разобрать», зажим, мусор, BOM, запись на диск.
        Config c;
        c.dim = 42;
        c.autostart = true;
        c.keyToggle = Bind(Bind::Ctrl | Bind::Shift, VK_F7);
        Config back = Config::parse(c.serialize());
        r.one("cfg-dim", back.dim, 42);
        r.flag("cfg-auto", back.autostart, true);
        r.flag("cfg-key", back.keyToggle.same(c.keyToggle) && back.keyLighter.same(c.keyLighter), true);
        r.one("cfg-clamp-hi", Config::parse("dim=900\n").dim, 100);
        r.one("cfg-clamp-lo", Config::parse("dim=-5\n").dim, 0);
        r.one("cfg-junk", Config::parse("dim=abc\n=\nпусто\nkey_toggle=zz\n\r\n").dim, Look::Liked);
        r.flag("cfg-junk-key", Config::parse("key_toggle=zz\n").keyToggle.same(Config().keyToggle), true);
        r.one("cfg-crlf", Config::parse("dim=33\r\nautostart=1\r\n").dim, 33);
        r.one("cfg-bom", Config::parse("\xEF\xBB\xBF" "dim=12\n").dim, 12);
        r.one("cfg-empty", Config::parse("").dim, Look::Liked);
        {
            wchar_t tmp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tmp);
            std::wstring path = std::wstring(tmp) + L"night-check-config.txt";
            bool saved = c.save(path);
            Config disk = Config::load(path);
            r.flag("cfg-disk", saved && disk.dim == 42 && disk.autostart, true);
            r.flag("cfg-no-tmp", GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, true);
            DeleteFileW(path.c_str());
            r.one("cfg-missing", Config::load(path).dim, Look::Liked);
        }

        // Раскладка окна: всё внутри окна, карточки не налезают друг на друга, кнопки лежат в своей карточке.
        {
            using namespace layout;
            const RECT win{0, 0, kWidth, kHeight};
            const RECT cards[] = {kLevelCard, kAutoCard, kKeysCard};
            bool inside = true, apart = true, keys = true;
            for (const RECT& a : cards) inside = inside && Inside(a, win);
            for (int i = 0; i < 3; i++)
                for (int j = i + 1; j < 3; j++) apart = apart && !Overlap(cards[i], cards[j]);
            for (int i = 0; i < 3; i++)
            {
                keys = keys && Inside(KeyButton(i), kKeysCard);
                for (int j = i + 1; j < 3; j++) keys = keys && !Overlap(KeyButton(i), KeyButton(j));
            }
            r.flag("lay-cards-inside", inside, true);
            r.flag("lay-cards-apart", apart, true);
            r.flag("lay-keys", keys, true);
            r.flag("lay-slider", Inside(kSlider, kLevelCard) && kSlider.right - kSlider.left > 2 * kSliderPad, true);
            r.flag("lay-hit", In(kKeysCard, kKeysCard.left, kKeysCard.top) && !In(kKeysCard, kKeysCard.right, kKeysCard.top), true);
            r.flag("lay-footer", kKeysCard.bottom + 40 <= kHeight, true);

            // Окно вопроса при закрытии: кнопки в ряд, внутри окна, текст над ними.
            const RECT ask{0, 0, kAskWidth, kAskHeight};
            bool buttons = Inside(kAskText, ask);
            for (int i = 0; i < 3; i++)
            {
                buttons = buttons && Inside(AskButton(i), ask) && !Overlap(AskButton(i), kAskText);
                for (int j = i + 1; j < 3; j++) buttons = buttons && !Overlap(AskButton(i), AskButton(j));
            }
            r.flag("lay-ask", buttons, true);
        }

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
}