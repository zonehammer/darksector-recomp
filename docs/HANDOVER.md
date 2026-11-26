# Dark Sector Recompiled: handover sheet

For the next agent picking this up. Written 26 Nov 2026 at commit `c6a000b`, plus this file.

## 1. What this is

This is a native Windows port of **Dark Sector** for Xbox 360 (Europe disc, title ID `445007D4`). It is built like the user's other recomps (WET, Conan, Fight Night Champion), and it was started from a copy of the Fight Night Champion repo:

- XenonRecomp translates the game's PowerPC code to C++ (`generated/ppc`).
- Our runtime supplies the kernel/XAM layer (`runtime/src/kernel`).
- Xenia's GPU and audio code (`thirdparty/xenia`, patched by `patches/`) renders and plays sound.

The template and lessons are in the user's `xbox360-recomp-playbook` skill. Read it first.

- **Repo:** https://github.com/zonehammer/darksector-recomp (public), branch `main`. VERSION is 0.0.1, with no release yet.
- **User's PC:**
  - repo at `G:\Dark Sector\darksector-recomp`;
  - extracted disc at `G:\Dark Sector\extracted`, already copied into the repo's `game\extracted` by `build.py --iso`;
  - ISO at `G:\Dark Sector\Dark Sector (Europe).iso`;
  - `G:\Dark Sector\_staging` is leftover split files, which the user can delete.
- **User's hardware:** RTX 4090 and an 8BitDo Ultimate controller (seen twice by SDL; the runtime already ignores the duplicate).

## 2. State right now

**Works on the user's PC (test 2, the log is `build\runtime\darksector_test2.log`):**
- The game boots. The D3 Publisher, Digital Extremes and Evolution Engine logos and the legal screen draw.
- Sound plays (XAudio2 and the FFmpeg XMA decoder).
- The title screen comes up in about 12 s. Before `c6a000b` it took about 40 s and hung (see 4.1).
- Pressing A or Start:
  - reads the profile;
  - auto-answers the storage device prompt;
  - creates the save `UserSaveContent` with its `settings` file;
  - goes through the menus.
- Starting a new game draws the opening scene ("LASRIA, U.S.S.R. - 1987 / Salvage Operation") correctly at 4K. No pipeline failures.

**Stops at:** a fatal C++ exception (`std::out_of_range`) during the first level load, about 1 s after the opening scene. It happens the same way on the PC and headless. See section 5, which is the next job.

## 3. Rules from the user (important)

- **Consult before deep bug hunts.** Don't chase a bug down a rabbit hole without asking. Present the finding plus options (A, B, ...), with a recommendation and a rough time cap. The user is sensitive to usage spent "chasing ghosts".
- **Keep testing to a minimum:**
  - get things working headless first;
  - when the user must test, give ONE test with as many checks as possible, numbered;
  - use switches (env vars or ini settings) so the user doesn't need a rebuild.
- **The user does not copy files.** After a test, read the logs and pictures yourself from their PC (connected folder `G:\Dark Sector`). Copy logs aside yourself before the next run (`darksector_testN.log`, `xenia_gpu_testN.log`). Turn `build\runtime\frames\frame_*.ppm` snapshots into small PNGs to look at them.
- **Always give full Command Prompt commands** for builds (below). Get the GitHub Windows CI (llvm-mingw and clang-cl) green before asking the user to build.
- **Legal:**
  - never commit game data or generated code;
  - never upload or share builds;
  - no trademarked logos (the user's own art is fine).
- **Naming:** use DS / darksector everywhere (env vars `DS_*`, log `darksector.log`, ini `darksector.ini`), never WET_ names in new code.
- **Releases:** versions must increase numerically. The user pushes tags; give them the `git tag` / `git push origin vX.Y.Z` commands and full release notes.
- **Off-limits:**
  - `H:\Conan-Recomp` is the user's personal build; never touch it;
  - don't patch Xenia's controller audio device picker.
- **Quick checks before any graphics debugging:**
  - adaptive_watch off (forced off for DS);
  - picture effects off;
  - 30 FPS vs higher;
  - 720p vs 4K;
  - `xenia_gpu.log` for "Failed to create graphics pipeline", or draws skipped that never stop;
  - compare with other ports.
- **Commit footer** (the attribution lines this session uses):
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EHWbK6tuMwDLc1BnzLXA8J
  ```
  Use your own session's lines if the system gives you different ones.

### The user's build and run commands

```
cd /d "G:\Dark Sector\darksector-recomp"
git pull
python tools\build.py
cd /d "G:\Dark Sector\darksector-recomp\build\runtime"
DarkSectorRecomp.exe --console
```

The first build on a new machine needs `python tools\build.py --iso "G:\Dark Sector\extracted"`. The exe writes `darksector.log`, `xenia_gpu.log` and `frames\` next to it.

## 4. What was done (and why)

Commits in order:

1. **`0cd6e51`** starts from the FNC repo:
   - names changed to DarkSector, title ID set, `GAME_DS` in `runtime/src/game.h`;
   - FNC-only gates in `options.cpp` and `xenia_bridge.cpp` widened to `GAME_FNC || GAME_DS`: adaptive_watch off, depth float24 in the pixel shader, `native_2x_msaa` off, no EDRAM wrap, real occlusion queries.
2. **`5a40558`** adds the kernel imports DS needs, in `runtime/src/kernel/ds_imports.cpp`:
   - NtCreateTimer / NtSetTimerEx / NtCancelTimer;
   - InterlockedFlushSList;
   - XamUser* and XamVoice*;
   - XamShowMessageBoxUI;
   - a few NetDll and XNet calls.
3. **`aea29f7`**: boot fixes found headless:
   - the setjmp/longjmp addresses in the config (libjpeg);
   - KeTlsAlloc zeroes the new slot on every thread;
   - NtReadFile completion routines are queued as APCs;
   - EVENT_WRITE_ZPD (occlusion) in the GPU stand-in;
   - `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` for clang-cl CI.
4. **`c6a000b`**: fake sockets. See 4.1.

### 4.1 The 30-s "CONTACT..." / "SERVER UNAVAILABLE" hang (fixed)

On the title screen DS opens two UDP sockets (one bound to port 1000).

- **Before:** `socket()` returned INVALID_SOCKET. The game retried for 30 s, showed SERVER UNAVAILABLE, then hung on a black screen (cancelling an XMsg overlapped every 30 s).
- **Xenia:** reaches gameplay in this game (Canary compatibility issue #622 says "state-gameplay") because it gives titles real host sockets.
- **Now** (`runtime/src/kernel/game_imports.cpp`, "sockets that open but reach nobody"):
  - socket, bind, setsockopt, ioctlsocket, getsockname and listen succeed;
  - send and sendto are dropped but report success;
  - recv, recvfrom and accept give WSAEWOULDBLOCK;
  - connect is refused;
  - select reports nothing readable and everything writable.
- XNetDnsLookup now answers like Xenia: a failed XNDNS, with its event signalled. DS never calls it.
- RtlRaiseException now logs MSVC C++ throws, with the thrown type names (`rtl.cpp`).

## 5. Next job: the crash at the first level load

```
FATAL: C++ exception thrown: object 700A2CD0, types: .?AVout_of_range@std@@ .?AVlogic_error@std@@ .?AVexception@std@@
guest call stack: 827DD72C 827D5044 82100B6C 8219E6E8 8228BBE4 8228C590 82274B00 82750BAC 8274DD94 82749DC8 821012E0 827ECFD8
```

The same "C++ exception thrown" line is in the user's test 2 log. Two cautions:
- The log contains stray binary bytes, so use `grep -a`.
- The "text: ... bad locale name" part of that line is junk the logger picked up from nearby memory. Ignore it.

What is known:

- **The throwing call:** `sub_82100B40` is `std::string::assign(const string& src, pos, n)` / substr. It calls `_Xran` (`sub_827D4FF8`) when `pos > src.size()`.
- **The caller:** `sub_8219E560` is a text parser that walks a big `key=value\n` text, with `{ }` blocks, using substr. It is called from `sub_8228BAA8` ← `sub_8228C320` ← `sub_822745B8` (the level/scene load).
- **At the throw** (headless, gdb):
  - the source string is at r30, an MSVC `std::string` (buffer pointer at +4, size at +0x14, capacity at +0x18);
  - its size is 5399 bytes (0x1517), its capacity 8092, and its text contains `...ance=0\n`;
  - pos is r27 = 0x5F80 (24448), so the parser has run far past the end of the text. On the PC run the values differ (r25=0x510, r27=0xAAE...), so it is data-dependent.
- **Strong clue:** the save `settings` file the game writes has garbled indentation. Lines inside `{ }` should start with 4 spaces, but some have 2 spaces plus 2 garbage bytes:
  ```
  searchSettings={
      players=4294967295
    XM-<scoreLimit=4294967295      <- bytes 2-3 of the indent are junk
     ?gameModeId=0
  ```
  So something in how the game builds or measures indentation and whitespace is mistranslated. The usual suspects:
  - a vector or memset path;
  - a 16-bit store or load (`sth`/`lhz`);
  - a `rlwinm`/`rlwimi` mask;
  - a byte-swapped halfword (`lhbrx`/`sthbrx`).
  The parser that crashes probably mis-skips whitespace for the same reason, and then reads past the end.
- **Suggested first steps (ask the user before going deep):**
  1. Find the function that writes the indent:
     - break on the settings file write (`NtWriteFile` to `s:\settings`) and walk back to the serializer;
     - or search the generated code for a loop storing 0x20 / 0x2020 / 0x20202020.
  2. Check that function's PPC instructions in the generated C++ against the XenonRecomp output for known-bad patterns.
  3. Compare with how the FNC and WET recomps handled similar issues. FNC needed fused `vmaddfp`/`vnmsubfp` fixes, which are in XenonRecomp at the pinned commit.
  4. Delete the bad `settings` file before retesting (`game\hdd\content\445007D4\00000001\UserSaveContent\settings` on the PC). A corrupted file may keep crashing even after the fix.

## 6. Other things noticed (not yet looked at)

- **Menus run at 56-60 FPS** although the target is 30. Gameplay pacing isn't checked yet. The FNC frame limiter may not apply to DS's present path.
- **"black frame" warnings** appear between logos and screens. They're probably normal fades.
- **The 7 "Unable to decode" warnings** at 82CC2308-233C are data, not code.

Not started:
- options and launcher polish (ultrawide, FOV);
- photo mode;
- mouse look;
- skip intro: the intro file names differ from FNC, so it needs updating;
- achievements;
- the README feature list.

## 7. Working headless (cloud container)

The cloud container has no GPU, so the game runs on Linux with `WET_RENDERER=OFF`. A GPU stand-in does the PM4 bookkeeping and a 60 Hz vblank. Every issue so far has reproduced headless, so use it before asking the user to test.

**Paths:** these were in the previous session's container and won't exist in a new one. Recreate them.

- **Repo clone:** `/home/claude/darksector-recomp`.
  - Submodules were cloned from the FNC repo's `.git/modules` with `-c protocol.file.allow=always`.
  - Set `submodule.*.ignore dirty` locally, because the build applies patches.
  - XenonRecomp is pinned at `ddd128bc`, Xenia at `95a5c3ee`.
- **Game data:** `/root/ds-work/extracted`, with `game/extracted` a symlink to it (git-ignored).
  - Get it from the user's PC: stage `G:\Dark Sector\extracted` (33 files, 1.4 GB) through the device bridge.
  - Two Cache files are over the 400 MB staging limit. The previous agent split them with the device shell into `_staging` and joined them in the container.
  - `default.xex` must also exist lower-case (the disc has `DEFAULT.XEX`).
- **Generated code:** run XenonRecomp with `config/DarkSector_config.toml` (see the playbook). The previous helper script copied only changed files into `generated/ppc`, so ninja stayed incremental.
  - The config result: 0 ERRORs, 0 traps.
  - It includes 300 switch tables plus 1 hand-made halfword table at 0x829C9000.
  - The register save/restore helpers and setjmp/longjmp addresses are in the config.
- **Configure and build:**
  ```
  cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DWET_RENDERER=OFF
  ninja -C build/linux
  ```
- **Run:**
  ```
  DS_AUTOPRESS="A,WAIT:4,A,WAIT:4,A,WAIT:4,START,WAIT:4,A,WAIT:8" DS_AUTOPRESS_DELAY=16 \
    timeout 120 build/linux/runtime/DarkSectorRecomp --game /root/ds-work/extracted --hdd /root/ds-work/run/hdd --log ds.log
  ```
  This script reaches the crash at about 41 s. Delete `run/hdd/content` first to start with a fresh save.
- **gdb:**
  - `handle SIGSEGV nostop noprint pass`, because write traps use SIGSEGV;
  - with ASLR off, guest base = `0x7ffef77f0000`;
  - in a guest function, `$rdi` = the PPCContext and `$rsi` = base;
  - PPCContext offsets: r3 at 0, r0 at 8, r1 at 16, r2 at 24, then rN at 8*N for N >= 4 (r31 at 0xF8);
  - break on `__imp__sub_XXXXXXXX` to stop in a guest function.
- **Don't kill processes with `pkill -f <pattern>`.** It can kill your own shell.

## 8. Useful switches (env vars, no rebuild)

| Variable | Effect |
|---|---|
| `DS_CONSOLE=1` (or `--console`) | Opens a live log window |
| `DS_FPS` | Sets the frame rate |
| `DS_RES_SCALE=1..3` | Resolution scale: 720p to 4K |
| `DS_ADAPTIVE_WATCH` | Adaptive watch (forced off for DS) |
| `DS_REAL_OCCLUSION` | Real occlusion queries |
| `DS_DEPTH24_PS` | Depth float24 conversion in the pixel shader |
| `DS_NATIVE_2X_MSAA` | Native 2x MSAA |
| `DS_RT_NO_EDRAM_WRAP` | No EDRAM wrap |
| `DS_AUTOPRESS` / `DS_AUTOPRESS_DELAY` | Scripted input |
| `DS_RECORD_INPUT` / `DS_REPLAY` | Record and replay input |
| `DS_NO_SKIP_INTRO` | Turns intro skipping off |
| `DS_TRACE_TLS` | TLS tracing |
| `DS_GPU_LOG_LEVEL` | Xenia GPU log level |

The full list: `grep -rhoE 'getenv\("DS_[A-Z_0-9]+"' runtime/src | sort -u`.

## 9. Credits to keep in the README

Credit XenonRecomp, Xenia / Xenia Canary, and FNC Reborn (for the FNC fixes inherited here). The game is © Digital Extremes / D3 Publisher.
