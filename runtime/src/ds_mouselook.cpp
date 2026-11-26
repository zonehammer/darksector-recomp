// Dark Sector: native mouse look. With mouse aim and "Native mouse look" on, the
// mouse turns the camera by an angle instead of being turned into a right-stick
// value: no stick dead zone, acceleration, response curve or turn-rate limit.
//
// sub_82447908(out, input, dt f1, ..., aiming r6) is the camera's turn from the
// right stick: input+28 / +36 the stick, the Camera.TurnRate / AimTurnRate
// settings (75 degrees a second by default) and a dozen acceleration and curve
// settings; it writes this frame's turn in degrees to out = { yaw, pitch, 0 },
// pitch flipped when the in-game invert option (input+412) is set. The mouse
// turn is added to that after the game's own code, with the same signs.
//
// Where this doesn't run (menus, cutscenes) the mouse goes back to the right
// stick (input.cpp: NativeLookActive, 0.3 s after the last call).
// DS_MOUSE_FLIP_X / DS_MOUSE_FLIP_Y = 1 flip an axis (first-test switches).
#include "game.h"
#if GAME_DS
#include "input.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace wet;

namespace
{
    bool Flip(const char* name)
    {
        const char* v = std::getenv(name);
        return v && *v && *v != '0';
    }
}

extern "C" void __imp__sub_82447908(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82447908)
{
    uint32_t out = ctx.r3.u32, input = ctx.r4.u32;
    {
        // Which camera code turns from the stick (logged once per caller).
        static std::atomic<int> s_callers{ 0 };
        static std::atomic<uint32_t> s_seen[8];
        uint32_t lr = uint32_t(ctx.lr);
        bool known = false;
        for (auto& v : s_seen)
            if (v.load() == lr)
                known = true;
        if (!known)
            if (int n = s_callers++; n < 8)
            {
                s_seen[n] = lr;
                Log("Camera: turn from the stick called from %08X (input %08X, aiming %u)", lr, input, ctx.r6.u32 & 0xFF);
            }
    }
    __imp__sub_82447908(ctx, base);
    if (!options::MouseAim() || !options::MouseNative() || !out || !input)
        return;
    double dx, dy;
    if (!TakeLookDelta(dx, dy) || (dx == 0 && dy == 0))
        return;
    // Sensitivity 50 (the default) = 0.066 degrees per pixel, as in WET.
    double degreesPerCount = 0.066 * options::MouseSensitivity() / 50.0;
    double yaw = dx * degreesPerCount;
    // Mouse down = positive pitch here (tested: the other sign was upside down).
    double pitch = dy * degreesPerCount;
    if (options::MouseInvertY())
        pitch = -pitch;
    if (PPC_LOAD_U8(input + 412))           // the game's own invert option, as for the stick
        pitch = -pitch;
    static const bool flipX = Flip("DS_MOUSE_FLIP_X"), flipY = Flip("DS_MOUSE_FLIP_Y");
    if (flipX)
        yaw = -yaw;
    if (flipY)
        pitch = -pitch;
    if (!std::isfinite(yaw) || !std::isfinite(pitch))
        return;
    yaw = std::clamp(yaw, -90.0, 90.0);
    pitch = std::clamp(pitch, -90.0, 90.0);
    auto add = [&](uint32_t a, double v) {
        uint32_t bits = PPC_LOAD_U32(a);
        float f;
        std::memcpy(&f, &bits, 4);
        f = float(f + v);
        std::memcpy(&bits, &f, 4);
        PPC_STORE_U32(a, bits);
    };
    add(out + 0, yaw);
    add(out + 4, pitch);
    static std::atomic<bool> logged{ false };
    if (!logged.exchange(true))
        Log("Mouse: native camera look in use (sensitivity %d)", options::MouseSensitivity());
}
#endif
