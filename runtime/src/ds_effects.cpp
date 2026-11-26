// Dark Sector: particle effects at 30 updates a second above 30 FPS.
//
// The game was made for 30 FPS. Above that, some particle effects (the sparks
// when the glaive hits a wall) flash on screen. Every particle system is
// simulated by sub_8211DBB8(system, dt) (the function behind the
// "SimulateParticles" profile marker), called from the systems' per-frame
// tick (sub_82121940, sub_824D46E8); it returns whether the system is still
// running (kept at system+2460).
//
// With effects_30fps on, each system is simulated at most once per 1/30 s of
// real time, with the time since its last simulation added up; on the frames
// between, the call is skipped and its last answer returned. Everything else
// (and drawing) still runs every frame. At 30 FPS or less every call goes
// through as before.
#include "game.h"
#if GAME_DS
#include "kernel/kernel.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct SystemState
    {
        Clock::time_point lastRun, lastCall;
        double pendingDt = 0;   // game time not yet simulated
        uint64_t result = 1;    // the last answer
    };
    std::mutex s_lock;
    std::unordered_map<uint32_t, SystemState> s_systems;
    Clock::time_point s_lastPrune;
    uint64_t s_skipped = 0, s_run = 0;

    // A little under 1/30 s, so frame-time jitter doesn't drop 60 FPS to 20 updates.
    constexpr auto kInterval = std::chrono::microseconds(30000);
    // A system not called for this long is a new one at the same address.
    constexpr auto kForget = std::chrono::milliseconds(100);
}

extern "C" void __imp__sub_8211DBB8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8211DBB8)
{
    if (!wet::options::Effects30())
    {
        __imp__sub_8211DBB8(ctx, base);
        return;
    }
    const uint32_t system = ctx.r3.u32;
    const double dt = ctx.f1.f64;
    const auto now = Clock::now();
    {
        std::lock_guard g(s_lock);
        if (now - s_lastPrune > std::chrono::seconds(2))
        {
            s_lastPrune = now;
            for (auto it = s_systems.begin(); it != s_systems.end();)
                it = now - it->second.lastCall > std::chrono::seconds(1) ? s_systems.erase(it) : std::next(it);
            static bool logged = false;
            if (!logged && s_run > 0)
            {
                logged = true;
                wet::Log("Effects: particle systems updated at most 30 times a second (%llu updates, %llu frames skipped so far)",
                         (unsigned long long)s_run, (unsigned long long)s_skipped);
            }
        }
        auto [it, added] = s_systems.try_emplace(system);
        SystemState& st = it->second;
        bool fresh = added || now - st.lastCall > kForget;
        st.lastCall = now;
        if (!fresh && now - st.lastRun < kInterval)
        {
            st.pendingDt += dt;
            ctx.r3.u64 = st.result;
            s_skipped++;
            return;
        }
        ctx.f1.f64 = fresh ? dt : st.pendingDt + dt;
        st.pendingDt = 0;
        st.lastRun = now;
        s_run++;
    }
    __imp__sub_8211DBB8(ctx, base);
    std::lock_guard g(s_lock);
    if (auto it = s_systems.find(system); it != s_systems.end())
        it->second.result = ctx.r3.u64;
}
#endif
