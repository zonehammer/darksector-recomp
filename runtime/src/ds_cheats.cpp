// Dark Sector testing cheats: the game's own GodMode and InfiniteAmmo console
// variables (left in from development, with a hidden cheat menu).
//
// Both are bool variables made by static initialisers (sub_82C7CCC0 for
// "GodMode", the next one for "InfiniteAmmo"): objects at 0x82D24170 and
// 0x82D241E4, value byte at +112. The game's damage check reads GodMode only
// for the player (sub_82589Cxx: "is the player" virtual, then the byte), so
// enemies still die.
//
// ApplyCheats runs once per frame (VdSwap) and writes the bytes when an option
// is on, and once more when it is turned off.
#include "game.h"
#if GAME_DS
#include "kernel/kernel.h"
#include "options.h"
#include "photo.h"
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
#endif
