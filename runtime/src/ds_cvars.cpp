// Dark Sector: the game's own console settings ("Graphics.FOV", "Graphics.PostFX",
// "Input.TurnRate", ... - about 330 of them), settable from game_settings.ini.
//
// Every setting is an object made by a static initialiser through
// sub_821923B0(object, category string, name string, flags): category
// std::string at +4, name at +32 (MSVC layout: text or pointer at +4, size
// at +20, capacity at +24), value at +112. Its type follows from its vtable:
// 0x820019B8 on/off (a byte), 0x820019CC a number (a float), the others a whole
// number or a choice from a list (an int, by index).
//
// The constructor override records every object. Once the game runs (from the
// first frame, VdSwap), game_settings.ini next to the game - lines
// "Category.Name = value" - is read, and again whenever it changes (checked
// once a second), and its values are written every frame, so the game can't
// put them back. game_settings_list.txt lists every setting with its type and
// current value (written once, a few seconds in).
#include "game.h"
#if GAME_DS
#include "kernel/kernel.h"
#include "log.h"
#include "ppc_context.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace wet;

namespace
{
    constexpr uint32_t kBoolVtable = 0x820019B8, kFloatVtable = 0x820019CC;

    std::mutex s_lock;
    std::vector<uint32_t> s_objects;

    std::string GuestString(uint32_t s)
    {
        uint32_t size = Ld32(s + 20), cap = Ld32(s + 24);
        if (size > 256)
            return {};
        uint32_t text = cap >= 16 ? Ld32(s + 4) : s + 4;
        std::string out;
        for (uint32_t i = 0; i < size; i++)
            out += char(Ld8(text + i));
        return out;
    }

    struct Setting
    {
        uint32_t object = 0;
        char type = 'i';   // 'b' on/off, 'f' number, 'i' whole number / choice
    };
    std::map<std::string, Setting> s_byName;   // lower-case "category.name"
    bool s_named = false;

    std::string Lower(std::string s)
    {
        for (auto& c : s)
            c = char(std::tolower((unsigned char)c));
        return s;
    }

    void NameAll()
    {
        std::lock_guard g(s_lock);
        for (uint32_t o : s_objects)
        {
            std::string name = GuestString(o + 4) + "." + GuestString(o + 32);
            uint32_t vt = Ld32(o);
            Setting st{ o, vt == kBoolVtable ? 'b' : vt == kFloatVtable ? 'f' : 'i' };
            s_byName[Lower(name)] = st;
        }
        s_named = true;
        Log("Game settings: %zu found (game_settings.ini sets them)", s_byName.size());
    }

    std::string ValueText(const Setting& s)
    {
        char text[64];
        if (s.type == 'b')
            std::snprintf(text, sizeof text, "%u", Ld8(s.object + 112));
        else if (s.type == 'f')
        {
            uint32_t bits = Ld32(s.object + 112);
            float f;
            std::memcpy(&f, &bits, 4);
            std::snprintf(text, sizeof text, "%g", f);
        }
        else
            std::snprintf(text, sizeof text, "%d", int32_t(Ld32(s.object + 112)));
        return text;
    }

    void WriteList()
    {
        std::FILE* f = std::fopen("game_settings_list.txt", "w");
        if (!f)
            return;
        std::fprintf(f, "# Dark Sector's own settings and their values now. Copy a line into\n"
                        "# game_settings.ini (next to the game) and change the value to set it;\n"
                        "# it applies within a second, while the game runs.\n"
                        "# on/off: 0 or 1. choice: a number (0 = the first choice).\n\n");
        for (auto& [lower, s] : s_byName)
        {
            std::string name = GuestString(s.object + 4) + "." + GuestString(s.object + 32);
            std::fprintf(f, "%-48s = %-10s # %s\n", name.c_str(), ValueText(s).c_str(),
                         s.type == 'b' ? "on/off" : s.type == 'f' ? "number" : "whole number / choice");
        }
        std::fclose(f);
        Log("Game settings: the list is in game_settings_list.txt");
    }

    struct Override
    {
        Setting setting;
        std::string name;
        double value = 0;
    };
    std::vector<Override> s_overrides;

    void ReadOverrides()
    {
        s_overrides.clear();
        std::FILE* f = std::fopen("game_settings.ini", "r");
        if (!f)
            return;
        char line[512];
        while (std::fgets(line, sizeof line, f))
        {
            std::string l = line;
            if (auto h = l.find('#'); h != std::string::npos)
                l.resize(h);
            auto eq = l.find('=');
            if (eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            std::string name = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
            if (name.empty() || value.empty())
                continue;
            auto it = s_byName.find(Lower(name));
            if (it == s_byName.end())
            {
                Log("Game settings: no setting called %s", name.c_str());
                continue;
            }
            Override o{ it->second, name, std::atof(value.c_str()) };
            s_overrides.push_back(o);
            Log("Game settings: %s = %s (was %s)", name.c_str(), value.c_str(), ValueText(it->second).c_str());
        }
        std::fclose(f);
    }
}

extern "C" void __imp__sub_821923B0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_821923B0)
{
    uint32_t object = ctx.r3.u32;
    __imp__sub_821923B0(ctx, base);
    std::lock_guard g(s_lock);
    s_objects.push_back(object);
}

namespace wet::cvars
{
    // Once a frame (VdSwap).
    void ApplyGameSettings()
    {
        using clock = std::chrono::steady_clock;
        static const auto start = clock::now();
        static auto lastCheck = clock::time_point{};
        static std::filesystem::file_time_type lastWrite{};
        static bool listed = false, hadFile = false;
        if (!s_named)
            NameAll();
        auto now = clock::now();
        if (!listed && now - start > std::chrono::seconds(5))
        {
            listed = true;
            WriteList();
        }
        if (now - lastCheck > std::chrono::seconds(1))
        {
            lastCheck = now;
            std::error_code ec;
            auto t = std::filesystem::last_write_time("game_settings.ini", ec);
            bool has = !ec;
            if (has != hadFile || (has && t != lastWrite))
            {
                hadFile = has;
                lastWrite = t;
                ReadOverrides();
            }
        }
        for (auto& o : s_overrides)
        {
            uint32_t a = o.setting.object + 112;
            if (o.setting.type == 'b')
                St8(a, o.value != 0 ? 1 : 0);
            else if (o.setting.type == 'f')
            {
                float f = float(o.value);
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                St32(a, bits);
            }
            else
                St32(a, uint32_t(int32_t(o.value)));
        }
    }
}
#endif
