// Dark Sector: PlayStation button prompts, from the game's own art.
//
// The game's interface settings object lists its button icons per platform -
// mCommonPlatformKeys (+124), mXboxPlatformKeys (+140), mPs3PlatformKeys (+156),
// each a std::vector of { Name=GAMEPAD_X, Material=/Interface/Graphics/
// PlatformKeys/ButtonA or ButtonCross, ... } - and the PS3 version's art
// (PS3Cross / Circle / Square / Triangle / L1 / L2 / R1 / R2 / DPad) is on the
// Xbox 360 disc too. The 360 build only reads the Xbox list. sub_82394DC8 is
// the object's field visitor (its vtable at 0x82019F2C): when it has read the
// lists, the Xbox and PlayStation lists are swapped in place (a swap, not a
// copy, so the game's own cleanup stays right), once per object.
//
// button_prompts: 0 automatic (PlayStation art with a PlayStation controller
// connected when the interface loads), 1 Xbox, 2 PlayStation.
#include "game.h"
#if GAME_DS
#include "input.h"
#include "kernel/kernel.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

#include <cstdio>
#include <cstring>
#include <mutex>

using namespace wet;

namespace
{
    constexpr uint32_t kXboxKeys = 140, kPs3Keys = 156, kVectorSize = 16;
    std::mutex s_lock;
    uint32_t s_xboxMaterial = 0, s_ps3Material = 0;

    bool WantPlayStation()
    {
        int mode = options::ButtonPrompts();
        return mode == 2 || (mode == 0 && PlayStationControllerConnected());
    }
}

extern "C" void __imp__sub_82394DC8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82394DC8)
{
    uint32_t object = ctx.r3.u32;
    __imp__sub_82394DC8(ctx, base);
    if (!object)
        return;
    uint32_t xbox = object + kXboxKeys, ps3 = object + kPs3Keys;
    uint32_t xboxBegin = Ld32(xbox + 4), ps3Begin = Ld32(ps3 + 4);
    if (!xboxBegin || !ps3Begin || Ld32(xbox + 8) == xboxBegin || Ld32(ps3 + 8) == ps3Begin)
        return;
    // Each entry: { ?, name std::string (+4: "GAMEPAD_X"...), material (+28), ... }.
    // The materials are shared resources, so the first entry's material tells
    // a list apart wherever it has been copied or moved to. Learnt from the
    // first load, before anything is swapped.
    std::lock_guard g(s_lock);
    if (!s_xboxMaterial)
    {
        s_xboxMaterial = Ld32(xboxBegin + 28);
        s_ps3Material = Ld32(ps3Begin + 28);
        Log("Prompts: interface key lists found (Xbox material %08X, PlayStation %08X)", s_xboxMaterial, s_ps3Material);
    }
    if (!WantPlayStation())
        return;
    if (Ld32(xboxBegin + 28) != s_xboxMaterial || Ld32(ps3Begin + 28) != s_ps3Material)
        return;   // already the PlayStation list (a copy of a swapped object)
    for (uint32_t i = 0; i < kVectorSize; i += 4)
    {
        uint32_t x = Ld32(xbox + i), p = Ld32(ps3 + i);
        St32(xbox + i, p);
        St32(ps3 + i, x);
    }
    static int logged = 0;
    if (logged++ < 3)
        Log("Prompts: PlayStation button art (the game's own PS3 icons), object %08X", object);
}
#endif
