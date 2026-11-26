// Dark Sector testing cheats: the game's own GodMode and InfiniteAmmo console
// variables (left in from development, with a hidden cheat menu).
//
// Both are bool variables made by static initialisers (sub_82C7CCC0 for
// "GodMode", the next one for "InfiniteAmmo"): objects at 0x82D24170 and
// 0x82D241E4, value byte at +112. The game asks for them through a global
// debug-settings object (static instance at 0x82D5FFD0, vtable 0x8202FCF0):
// sub_82589C20 is IsGodMode and sub_82589CD0 IsInfiniteAmmo, each
// "CheatsAllowed() && variable". CheatsAllowed (vtable +220, sub_82719E60)
// is hard-wired to false in the retail game, so the variables alone do
// nothing: the two getters are overridden below to answer yes while the option
// is on, and run the game's own code otherwise.
//
// ApplyCheats runs once per frame (VdSwap) and also sets the variables, so
// the game's hidden cheat menu shows the same state.
#include "game.h"
#if GAME_DS
#include "kernel/kernel.h"
#include "options.h"
#include "photo.h"
#include "ppc_context.h"
#include <cstring>

namespace wet::cheats
{
    namespace
    {
        constexpr uint32_t kGodMode = 0x82D24170 + 112;
        constexpr uint32_t kInfiniteAmmo = 0x82D241E4 + 112;
        int s_lastGod = -1, s_lastAmmo = -1;

        void StF32(uint32_t addr, float v)
        {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            St32(addr, bits);
        }

        void Apply(uint32_t addr, bool on, int& last, const char* name)
        {
            if (on)
                St8(addr, 1);
            if (int now = on ? 1 : 0; now != last)
            {
                if (!on)
                    St8(addr, 0);
                if (last != -1)
                    photo::SetToast(std::string(name) + (on ? " on" : " off"));
                Log("Cheats: %s %s", name, on ? "on" : "off");
                last = now;
            }
        }
    }

    // Graphics.FOV (made by sub_82C81A10: object 0x82D298C0, float at
    // +112, default 65): the camera's field of view, read wherever the
    // camera builds its projection. Written while it differs from the game's
    // own 65, and once more when set back.
    namespace
    {
        constexpr uint32_t kFov = 0x82D298C0 + 112;
        int s_lastFov = 65;
        void ApplyFov()
        {
            int fov = options::CameraFov();
            if (fov != 65)
                StF32(kFov, float(fov));
            if (fov != s_lastFov)
            {
                if (fov == 65)
                    StF32(kFov, 65.0f);
                Log("Camera: field of view %d", fov);
                s_lastFov = fov;
            }
        }
    }

    void ApplyCheats()
    {
        ApplyFov();
        Apply(kGodMode, options::GodMode(), s_lastGod, "God mode");
        Apply(kInfiniteAmmo, options::InfiniteAmmo(), s_lastAmmo, "Infinite ammo");
    }
}

extern "C" void __imp__sub_82589C20(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82589CD0(PPCContext& ctx, uint8_t* base);

// IsGodMode(): yes while the god_mode option is on.
PPC_FUNC(sub_82589C20)
{
    if (wet::options::GodMode())
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            wet::Log("Cheats: the game checked god mode - on");
        }
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_82589C20(ctx, base);
}

// IsInfiniteAmmo(): yes while the infinite_ammo option is on.
PPC_FUNC(sub_82589CD0)
{
    if (wet::options::InfiniteAmmo())
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            wet::Log("Cheats: the game checked infinite ammo - on");
        }
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_82589CD0(ctx, base);
}
#endif
