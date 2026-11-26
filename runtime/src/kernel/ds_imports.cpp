// Imports first needed by Dark Sector. Behaviour follows Xenia (kernel/xam,
// xboxkrnl).
#include "kernel.h"
#include "objects.h"
#include "os.h"
#include "threads.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace wet;

// ---------------------------------------------------------------------------
// InterlockedFlushSList(SLIST_HEADER*): empties the list, returns its old
// first entry (header layout: see InterlockedPopEntrySList in sync.cpp).
// ---------------------------------------------------------------------------
GUEST_IMPORT(InterlockedFlushSList)
{
    auto* p = reinterpret_cast<uint64_t*>(HostPtr(Arg(ctx, 0)));
    for (;;)
    {
        uint64_t raw = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        uint64_t v = __builtin_bswap64(raw);
        uint32_t first = uint32_t(v >> 32);
        if (!first)
            return Ret(ctx, 0);
        uint64_t nv = v & 0xFFFF;   // keep the sequence
        uint64_t nraw = __builtin_bswap64(nv);
        if (__atomic_compare_exchange_n(p, &raw, nraw, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return Ret(ctx, first);
    }
}

// ---------------------------------------------------------------------------
// Timers: NtCreateTimer / NtSetTimerEx / NtCancelTimer. A timer is an event
// (notification or synchronization) signalled by a host thread when it's due,
// and again every period. APC routines aren't run (logged once).
// ---------------------------------------------------------------------------
namespace
{
    struct TimerState
    {
        std::shared_ptr<KEvent> event;
        std::atomic<uint64_t> generation{ 0 };
    };
    std::mutex s_timerLock;
    std::unordered_map<KEvent*, std::shared_ptr<TimerState>> s_timers;

    void SignalTimer(const std::shared_ptr<KEvent>& e)
    {
        {
            std::lock_guard lock(DispatchLock());
            e->Set(true);
        }
        DispatchNotify(e.get());
    }
}

// NtCreateTimer(handle*, object_attributes, timer_type): 0 notification, 1 synchronization
GUEST_IMPORT(NtCreateTimer)
{
    auto e = std::make_shared<KEvent>(Arg(ctx, 2) == 0, false);
    auto t = std::make_shared<TimerState>();
    t->event = e;
    {
        std::lock_guard lock(s_timerLock);
        s_timers[e.get()] = t;
    }
    St32(Arg(ctx, 0), CreateHandle(e));
    Ret(ctx, STATUS_SUCCESS);
}

// NtSetTimerEx(handle, due_time* (100 ns: negative relative, positive absolute),
//              apc_routine, apc_mode, apc_argument, resume, period_ms, previous_state*)
GUEST_IMPORT(NtSetTimerEx)
{
    auto e = LookupHandleAs<KEvent>(Arg(ctx, 0), ObjType::Event);
    std::shared_ptr<TimerState> t;
    if (e)
    {
        std::lock_guard lock(s_timerLock);
        auto it = s_timers.find(e.get());
        if (it != s_timers.end())
            t = it->second;
    }
    if (!t)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    int64_t due = Arg(ctx, 1) ? int64_t(Ld64(Arg(ctx, 1))) : 0;
    uint32_t apc = Arg(ctx, 2), period = Arg(ctx, 6), prevPtr = Arg(ctx, 7);
    if (apc)
    {
        static std::atomic<int> logged{ 0 };
        if (logged++ < 3)
            Log("NtSetTimerEx: APC routine %08X not run (timers signal their event only)", apc);
    }
    if (prevPtr)
        St32(prevPtr, e->signaled ? 1 : 0);
    {
        std::lock_guard lock(DispatchLock());
        e->Set(false);
    }
    uint64_t delayNs = due < 0 ? uint64_t(-due) * 100
                     : due > 0 ? (uint64_t(due) > CurrentFileTime() ? (uint64_t(due) - CurrentFileTime()) * 100 : 0)
                               : 0;
    uint64_t gen = ++t->generation;
    std::thread([t, gen, delayNs, period] {
        auto next = std::chrono::steady_clock::now() + std::chrono::nanoseconds(delayNs);
        for (;;)
        {
            std::this_thread::sleep_until(next);
            if (t->generation.load() != gen)
                return;
            SignalTimer(t->event);
            if (!period)
                return;
            next += std::chrono::milliseconds(period);
        }
    }).detach();
    Ret(ctx, STATUS_SUCCESS);
}

// NtCancelTimer(handle, previous_state*)
GUEST_IMPORT(NtCancelTimer)
{
    auto e = LookupHandleAs<KEvent>(Arg(ctx, 0), ObjType::Event);
    if (!e)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    {
        std::lock_guard lock(s_timerLock);
        auto it = s_timers.find(e.get());
        if (it != s_timers.end())
            ++it->second->generation;
    }
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), e->signaled ? 1 : 0);
    Ret(ctx, STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------
// XAM: users, voice. One local signed-in player, no friends, no voice.
// ---------------------------------------------------------------------------
// XamUserCheckPrivilege(user, privilege, result*): allowed, as Xenia
GUEST_IMPORT(XamUserCheckPrivilege)
{
    if (Arg(ctx, 2))
        St32(Arg(ctx, 2), 1);
    Ret(ctx, ERROR_SUCCESS);
}

// XamUserAreUsersFriends(user, xuids*, count, result*, overlapped)
GUEST_IMPORT(XamUserAreUsersFriends)
{
    if (Arg(ctx, 3))
        St32(Arg(ctx, 3), 0);
    Ret(ctx, ERROR_SUCCESS);
}

GUEST_IMPORT(XamVoiceCreate) { Ret(ctx, ERROR_ACCESS_DENIED); }
GUEST_IMPORT(XamVoiceSubmitPacket) { Ret(ctx, ERROR_ACCESS_DENIED); }
GUEST_IMPORT(XamVoiceClose) { Ret(ctx, 0); }
GUEST_IMPORT(XamVoiceHeadsetPresent) { Ret(ctx, 0); }

// The Guide's player screens: there's no Guide.
GUEST_IMPORT(XamShowPlayersUI) { Log("XamShowPlayersUI: no guide UI; ignored"); Ret(ctx, ERROR_SUCCESS); }
GUEST_IMPORT(XamShowPlayerReviewUI) { Log("XamShowPlayerReviewUI: no guide UI; ignored"); Ret(ctx, ERROR_SUCCESS); }
