// Imports first needed by Conan. Behaviour follows Xenia (kernel/xam, xboxkrnl).
#include "kernel.h"
#include "os.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

using namespace wet;

// XGetVideoMode(X_VIDEO_MODE*): 1280x720 progressive widescreen HD, 60 Hz.
GUEST_IMPORT(XGetVideoMode)
{
    uint32_t m = Arg(ctx, 0);
    Zero(m, 0x30);
    St32(m + 0x00, 1280);
    St32(m + 0x04, 720);
    St32(m + 0x08, 0);          // interlaced
    St32(m + 0x0C, 1);          // widescreen
    St32(m + 0x10, 1);          // high definition
    float hz = 60.0f;
    uint32_t bits;
    std::memcpy(&bits, &hz, 4);
    St32(m + 0x14, bits);
    St32(m + 0x18, 1);          // video standard: NTSC
    St32(m + 0x1C, 0x4A);
    St32(m + 0x20, 0x01);
}

// 6 is VGA; titles treat anything other than 3/4/6/8 as PAL.
GUEST_IMPORT(XGetAVPack) { Ret(ctx, 6); }

GUEST_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), 0);
    Ret(ctx, ERROR_SUCCESS);
}

// XamContentGetDeviceData(device_id, X_CONTENT_DEVICE_DATA*)
GUEST_IMPORT(XamContentGetDeviceData)
{
    uint32_t d = Arg(ctx, 1);
    if (Arg(ctx, 0) != 1)
        return Ret(ctx, ERROR_DEVICE_NOT_CONNECTED);
    Zero(d, 0x50);
    St32(d + 0x00, 1);                       // device id
    St32(d + 0x04, 1);                       // hard drive
    St64(d + 0x08, 20ull << 30);             // total bytes
    St64(d + 0x10, 16ull << 30);             // free bytes
    const char* name = "Hard Drive";
    for (int i = 0; name[i]; i++)
        St16(d + 0x18 + i * 2, uint16_t(name[i]));
    Ret(ctx, ERROR_SUCCESS);
}

// Sessions (online play): a dummy handle and object, as Xenia does.
GUEST_IMPORT(XamSessionCreateHandle)
{
    St32(Arg(ctx, 0), 0xCAFEDEAD);
    Ret(ctx, ERROR_SUCCESS);
}
GUEST_IMPORT(XamSessionRefObjByHandle)
{
    St32(Arg(ctx, 1), 0xDEADF00D);
    Ret(ctx, ERROR_SUCCESS);
}

// Leaderboards and gamer cards need Xbox Live.
GUEST_IMPORT(XamUserCreateStatsEnumerator) { Ret(ctx, ERROR_FUNCTION_FAILED); }
GUEST_IMPORT(XamShowGamerCardUIForXUID) { Ret(ctx, ERROR_FUNCTION_FAILED); }

GUEST_IMPORT(XamShowDirtyDiscErrorUI)
{
    Log("XamShowDirtyDiscErrorUI: the game could not read a file from the disc; check game\\extracted");
    LogFlush();
    os::ShowFatalMessage("Dark Sector Recomp stopped: the game could not read one of its files.\n\n"
                         "Check that the game was extracted completely (see darksector.log).");
    os::ExitProcess(1);
}

// Structured exception handler for C code: not used by recompiled code, which
// has no SEH unwinding. ExceptionContinueSearch.
GUEST_IMPORT(__C_specific_handler) { Ret(ctx, 1); }

// ---------------------------------------------------------------------------
// Network: sockets that open but reach nobody.
//
// The real console always gives a title working sockets, even with no cable;
// Xenia does too (host sockets). Dark Sector opens one on its title screen
// ("CONTACT...") and, when socket() fails, retries for 30 s, shows SERVER
// UNAVAILABLE and then waits for it forever. So: socket() hands out a handle,
// bind/setsockopt/ioctlsocket succeed, sends are dropped (reported as sent),
// and receives report "nothing yet" (WSAEWOULDBLOCK); connect is refused.
// ---------------------------------------------------------------------------
constexpr uint32_t kSocketError = 0xFFFFFFFF;
constexpr uint32_t kWsaEWouldBlock = 10035, kWsaEConnRefused = 10061, kWsaENotSock = 10038;

namespace
{
    struct FakeSocket { uint32_t type = 0; uint16_t port = 0; };
    std::mutex s_sockLock;
    std::unordered_map<uint32_t, FakeSocket> s_sockets;
    uint32_t s_nextSocket = 0x5C000004;
    uint16_t s_nextPort = 49152;
    thread_local uint32_t t_wsaError = 0;

    bool SocketOk(uint32_t s)
    {
        std::lock_guard lock(s_sockLock);
        if (s_sockets.count(s))
            return true;
        t_wsaError = kWsaENotSock;
        return false;
    }
    void NetLog(const char* fmt, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0)
    {
        static std::atomic<int> logged{0};
        if (logged++ < 40)
            Log(fmt, a, b, c);
    }
}

// NetDll_socket(caller, af, type, protocol)
GUEST_IMPORT(NetDll_socket)
{
    std::lock_guard lock(s_sockLock);
    uint32_t s = s_nextSocket;
    s_nextSocket += 4;
    s_sockets[s].type = Arg(ctx, 2);
    NetLog("Network: socket(type %u, protocol %u) -> %08X (no network: it reaches nobody)", Arg(ctx, 2), Arg(ctx, 3), s);
    Ret(ctx, s);
}
GUEST_IMPORT(NetDll_closesocket)
{
    std::lock_guard lock(s_sockLock);
    s_sockets.erase(Arg(ctx, 1));
    Ret(ctx, 0);
}
GUEST_IMPORT(NetDll_WSAGetLastError) { Ret(ctx, t_wsaError ? t_wsaError : kWsaEWouldBlock); }

// bind(caller, s, sockaddr*, len): remembers the port (0 = pick one)
GUEST_IMPORT(NetDll_bind)
{
    uint32_t s = Arg(ctx, 1), addr = Arg(ctx, 2);
    if (!SocketOk(s))
        return Ret(ctx, kSocketError);
    uint16_t port = addr ? uint16_t((Ld8(addr + 2) << 8) | Ld8(addr + 3)) : 0;
    std::lock_guard lock(s_sockLock);
    s_sockets[s].port = port ? port : s_nextPort++;
    NetLog("Network: bind(%08X, port %u)", s, s_sockets[s].port);
    Ret(ctx, 0);
}
// getsockname(caller, s, sockaddr*, len*): 127.0.0.1 and the bound port
GUEST_IMPORT(NetDll_getsockname)
{
    uint32_t s = Arg(ctx, 1), addr = Arg(ctx, 2), lenPtr = Arg(ctx, 3);
    if (!SocketOk(s))
        return Ret(ctx, kSocketError);
    uint16_t port;
    {
        std::lock_guard lock(s_sockLock);
        port = s_sockets[s].port;
    }
    if (addr)
    {
        Zero(addr, 16);
        St8(addr + 1, 2);                      // AF_INET (big-endian u16 family)
        St8(addr + 2, uint8_t(port >> 8));
        St8(addr + 3, uint8_t(port));
        St32(addr + 4, 0x7F000001);
    }
    if (lenPtr)
        St32(lenPtr, 16);
    Ret(ctx, 0);
}
GUEST_IMPORT(NetDll_setsockopt) { Ret(ctx, SocketOk(Arg(ctx, 1)) ? 0 : kSocketError); }
GUEST_IMPORT(NetDll_ioctlsocket) { Ret(ctx, SocketOk(Arg(ctx, 1)) ? 0 : kSocketError); }
GUEST_IMPORT(NetDll_listen) { Ret(ctx, SocketOk(Arg(ctx, 1)) ? 0 : kSocketError); }
GUEST_IMPORT(NetDll_accept)
{
    t_wsaError = kWsaEWouldBlock;
    Ret(ctx, kSocketError);
}
GUEST_IMPORT(NetDll_connect)
{
    NetLog("Network: connect(%08X): refused (no network)", Arg(ctx, 1));
    t_wsaError = kWsaEConnRefused;
    Ret(ctx, kSocketError);
}
// send(caller, s, buf, len, flags) / sendto(caller, s, buf, len, flags, to, tolen): dropped
GUEST_IMPORT(NetDll_send) { Ret(ctx, SocketOk(Arg(ctx, 1)) ? Arg(ctx, 3) : kSocketError); }
GUEST_IMPORT(NetDll_sendto)
{
    uint32_t s = Arg(ctx, 1), to = Arg(ctx, 5);
    if (!SocketOk(s))
        return Ret(ctx, kSocketError);
    if (to)
        NetLog("Network: sendto(%08X, %u bytes, to %08X): dropped", s, Arg(ctx, 3), Ld32(to + 4));
    Ret(ctx, Arg(ctx, 3));
}
GUEST_IMPORT(NetDll_recv)
{
    t_wsaError = kWsaEWouldBlock;
    Ret(ctx, kSocketError);
}
GUEST_IMPORT(NetDll_recvfrom)
{
    t_wsaError = kWsaEWouldBlock;
    Ret(ctx, kSocketError);
}
// select(caller, nfds, read, write, except, timeval*): nothing readable; a
// socket is always writable. Waits out the timeout (at most 100 ms per call).
GUEST_IMPORT(NetDll_select)
{
    uint32_t readSet = Arg(ctx, 2), writeSet = Arg(ctx, 3), exceptSet = Arg(ctx, 4), tv = Arg(ctx, 5);
    uint32_t ready = writeSet ? std::min<uint32_t>(Ld32(writeSet), 64) : 0;
    if (readSet)
        St32(readSet, 0);
    if (exceptSet)
        St32(exceptSet, 0);
    if (!ready)
    {
        uint64_t us = tv ? uint64_t(Ld32(tv)) * 1000000ull + Ld32(tv + 4) : 100000ull;
        os::SleepNs(std::min<uint64_t>(us, 100000ull) * 1000ull);
    }
    Ret(ctx, ready);
}
// __WSAFDIsSet(s, fd_set*)
GUEST_IMPORT(NetDll___WSAFDIsSet)
{
    uint32_t s = Arg(ctx, 0), set = Arg(ctx, 1), found = 0;
    for (uint32_t i = 0, n = set ? std::min<uint32_t>(Ld32(set), 64) : 0; i < n; i++)
        if (Ld32(set + 4 + i * 4) == s)
            found = 1;
    Ret(ctx, found);
}

// NetDll_XNetGetTitleXnAddr(caller, XNADDR*): loopback, as Xenia (a zero MAC
// breaks some network libraries).
GUEST_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    uint32_t a = Arg(ctx, 1);
    Zero(a, 0x24);
    St32(a + 0, 0x7F000001);
    for (int i = 0; i < 6; i++)
        St8(a + 10 + i, 0xCC);
    Ret(ctx, 4); // XNET_GET_XNADDR_STATIC
}

// More of the network library (Dark Sector): no network, as above.
GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus) { Ret(ctx, 0); }   // no cable
GUEST_IMPORT(NetDll_XNetQosRelease) { Ret(ctx, 0); }
GUEST_IMPORT(NetDll_XNetQosLookup) { Ret(ctx, 0x8007274C); }      // WSAETIMEDOUT as HRESULT
GUEST_IMPORT(NetDll_XNetQosListen) { Ret(ctx, 0x8007274C); }
GUEST_IMPORT(NetDll_XNetXnAddrToInAddr) { Ret(ctx, 0x80070057); }  // E_INVALIDARG
