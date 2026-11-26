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
// The constructor override records every object. Settings are applied at
// startup, before the game sets up its graphics (VdInitializeEngines), and
// written again every frame so the game can't put them back. They are never
// changed while the game runs: some (Graphics.SimplifiedBloom) crash the game
// if changed after its effects are set up.
//
// Two sources:
//  - the F1 "Game" tab's switches (kSwitches below), kept in game_switches.ini
//    (written by the runtime; a change applies at the next start);
//  - game_settings.ini, "Category.Name = value" lines (diagnostics: any
//    setting), read once at startup.
// game_settings_list.txt lists every setting with its type and value.
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

    void AddOverride(const std::string& name, double value, const char* from)
    {
        auto it = s_byName.find(Lower(name));
        if (it == s_byName.end())
        {
            Log("Game settings: no setting called %s", name.c_str());
            return;
        }
        Log("Game settings: %s = %g (%s; was %s)", name.c_str(), value, from, ValueText(it->second).c_str());
        s_overrides.push_back({ it->second, name, value });
    }

    void ReadSettingsFile()
    {
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
            auto trim = [](std::string t) {
                size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
            };
            std::string name = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
            if (name.empty() || value.empty() || name.find(' ') != std::string::npos)
                continue;
            AddOverride(name, std::atof(value.c_str()), "game_settings.ini");
        }
        std::fclose(f);
    }

    // The F1 "Game" tab: on/off switches over the game's own settings. "on" is
    // the switch as shown; setting = on ? onValue : !onValue.
    struct Switch
    {
        const char* key;      // in game_switches.ini
        const char* label;
        const char* help;
        const char* setting;  // the game's setting
        bool onValue;         // its value when the switch is on
        bool gameDefault;     // the switch as the game has it
    };
    constexpr Switch kSwitches[] = {
        { "post_effects", "Post effects", "The levels' colour filters (the grey look), bloom, blur and depth of field. Off: plain picture.", "Graphics.PostFX", true, true },
        { "full_bloom", "Full bloom", "The game's full glow around bright lights. Off: its cheaper, simpler bloom.", "Graphics.SimplifiedBloom", false, true },
        { "cascaded_shadows", "Cascaded shadows", "The large sun / main-light shadows (a 2048x2048 shadow map, the most costly effect at high resolutions).", "Graphics.EnableCSM", true, true },
        { "full_size_effects", "Full-size effects", "Effects drawn at full size. Off: the game's smaller render targets (faster, softer).", "Graphics.ReducedTargetSize", false, true },
        { "blurred_shadows", "Soft shadow edges", "Blurs the shadows on screen. Off: sharper edges, a little faster.", "Graphics.BlurScreenShadow", true, true },
        { "gameplay_dof", "Depth of field in play", "Background blur in gameplay too, not only in cutscenes.", "Graphics.CinematicDOFOnly", false, false },
    };
    constexpr int kSwitchCount = int(sizeof(kSwitches) / sizeof(kSwitches[0]));
    bool s_switch[kSwitchCount], s_switchAtStart[kSwitchCount];
    std::once_flag s_switchesLoaded;

    void LoadSwitches()
    {
        std::call_once(s_switchesLoaded, [] {
            for (int i = 0; i < kSwitchCount; i++)
                s_switch[i] = kSwitches[i].gameDefault;
            if (std::FILE* f = std::fopen("game_switches.ini", "r"))
            {
                char line[256], key[128];
                int v;
                while (std::fgets(line, sizeof line, f))
                    if (std::sscanf(line, " %127[a-z_] = %d", key, &v) == 2)
                        for (int i = 0; i < kSwitchCount; i++)
                            if (!std::strcmp(key, kSwitches[i].key))
                                s_switch[i] = v != 0;
                std::fclose(f);
            }
            for (int i = 0; i < kSwitchCount; i++)
                s_switchAtStart[i] = s_switch[i];
        });
    }

    void SaveSwitches()
    {
        std::FILE* f = std::fopen("game_switches.ini", "w");
        if (!f)
            return;
        std::fprintf(f, "# Dark Sector's own settings (F1 > Game). Written by the game; 1 = on.\n");
        for (int i = 0; i < kSwitchCount; i++)
            std::fprintf(f, "%s = %d\n", kSwitches[i].key, s_switch[i] ? 1 : 0);
        std::fclose(f);
    }

    void ApplyNow()
    {
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

extern "C" void __imp__sub_821923B0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_821923B0)
{
    uint32_t object = ctx.r3.u32;
    __imp__sub_821923B0(ctx, base);
    std::lock_guard g(s_lock);
    s_objects.push_back(object);
}

namespace wet::gamesettings
{
    // Once, before the game sets up its graphics (VdInitializeEngines): name
    // every setting, take the switches and game_settings.ini, write them.
    void ApplyAtStartup()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        NameAll();
        LoadSwitches();
        for (int i = 0; i < kSwitchCount; i++)
            if (s_switchAtStart[i] != kSwitches[i].gameDefault)
                AddOverride(kSwitches[i].setting, (s_switchAtStart[i] ? kSwitches[i].onValue : !kSwitches[i].onValue) ? 1 : 0,
                            kSwitches[i].label);
        ReadSettingsFile();
        ApplyNow();
    }

    // Once a frame (VdSwap): the same values again, so the game keeps them.
    void ApplyGameSettings()
    {
        ApplyAtStartup();
        static const auto start = std::chrono::steady_clock::now();
        static bool listed = false;
        if (!listed && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
        {
            listed = true;
            WriteList();
        }
        ApplyNow();
    }

    // F1 "Game" tab.
    int SwitchCount() { return kSwitchCount; }
    const char* SwitchLabel(int i) { return kSwitches[i].label; }
    const char* SwitchHelp(int i) { return kSwitches[i].help; }
    bool SwitchOn(int i)
    {
        LoadSwitches();
        return s_switch[i];
    }
    void SetSwitch(int i, bool on)
    {
        LoadSwitches();
        s_switch[i] = on;
        SaveSwitches();
    }
    bool SwitchesNeedRestart()
    {
        LoadSwitches();
        for (int i = 0; i < kSwitchCount; i++)
            if (s_switch[i] != s_switchAtStart[i])
                return true;
        return false;
    }
}
#endif
