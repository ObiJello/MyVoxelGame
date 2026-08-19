# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Network Info

- **Mac Public IP:** 108.35.220.113 (was 74.105.150.36; residential IP — re-check with `curl https://api.ipify.org` and update `src/common/core/FriendsServiceConfig.hpp` when it rotates)

## Git Policy

- **Never add Co-Authored-By lines** to commit messages. No Claude attribution in commits.
- Do not build unless asked. Do not push unless asked.

## Build Policy

**Do NOT build after making changes.** Instead, say "try building with new changes and if there are any errors let me know." Only run the build command when the user reports errors and asks you to build to diagnose them.

## Build Commands

The project uses CMake with Ninja (CLion is the primary IDE).

### Quick Build Commands
```bash
# Build (debug, CLion-style)
cmake --build cmake-build-debug --target MyVoxelGame -j$(sysctl -n hw.ncpu)

# Build (release)
cmake --build cmake-build-release --target MyVoxelGame -j$(sysctl -n hw.ncpu)

# Configure from scratch (if needed)
cmake -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Distribution Build
For distributing to users (uploads dSYMs to Sentry, builds universal binary):
- CLion profile: add `-DJALIN=ON` to CMake options
- Environment: `VULKAN_SDK=/Users/obey/VulkanSDK/1.4.341.1/macOS`
- Setting `VULKAN_SDK` enables universal binary (arm64 + x86_64) with Vulkan support

### Vulkan SDK
- Location: `/Users/obey/VulkanSDK/1.4.341.1/macOS`
- Homebrew MoltenVK is arm64-only; use the Vulkan SDK for universal builds

### Running the Game
```bash
# macOS (app bundle)
./build/bin/MyVoxelGame.app/Contents/MacOS/MyVoxelGame

# Linux/Windows
./build/bin/MyVoxelGame
```

### Command-line arguments

All parsed in `src/platform/PlatformMain.cpp:Run()`. The launcher (`src/launcher/LauncherApp.cpp`) builds these from the user's settings + Play/Join buttons and passes them to the game at launch (via `system("open ... --args ...")` on macOS, `ShellExecuteA` on Windows). Anything you add here, you also need a `build*Arg` lambda + UI control in the launcher.

| Flag | Value | Meaning |
|---|---|---|
| `--vulkan` | (none) | Use the Vulkan render backend instead of OpenGL. Launcher: "Use Vulkan" checkbox in settings. |
| `--crash-test` | (none) | Trigger a deliberate crash at startup — for testing the Sentry pipeline. Not exposed in the launcher. |
| `--server` | `host[:port]` | Connect to a remote dedicated server instead of running the integrated one. Defaults to port 25565. Launcher: "Join Server" dialog (IP + port). |
| `--name` | `<string>` | Player name. Empty/missing → server auto-assigns "PlayerN" based on connection ID. Launcher: "Username" field. |
| `--color` | `<slug>` | Player stick-figure colour. Slug is one of `default`, `red`, `orange`, `yellow`, `blue`, `purple`, `pink`, `white`, `black`, `brown` (case-insensitive). Empty/missing/`default` → neon green (`#00FF3C`). Launcher: swatch grid in settings. |
| `--session` | `<token>` | Friends-service session token from a launcher login. Missing → guest (Friends UI disabled). Launcher: added automatically when logged in. |
| `--account-id` | `<n>` | Friends-service account id paired with `--session`. |
| `--friends-service` | `host[:port]` | Overrides the friends-service address (defaults in `src/common/core/FriendsServiceConfig.hpp`). Launcher: forwarded from the `friends_service` key in `launcher.json`. |

**Player color flow** (added 2026-05): launcher persists slug to `launcher.json` `"player_color"` → passes `--color <slug>` on launch → `PlatformMain` parses via `Game::ParsePlayerColorName` (`src/common/entity/PlayerColors.{hpp,cpp}`) → stored on `ClientPlayer.color` → forwarded to network as a uint8_t via `NetworkClient::SetPlayerColor` → tail-appended byte after username in the `LoginStart` packet → server's `LoginPacketListener::onLoginStart` calls `m_connection.SetPlayerColor(packet.colorId)` → IntegratedServer's `OnPlayerJoined` reads `connection->GetPlayerColor()` onto the new `ServerPlayer.colorId` → broadcast in both `PlayerInfoS2C ADD` paths (existing-players-to-new-client and new-player-to-all) → client writes onto `RemotePlayer.color` → `PlayerRenderer` looks up the RGB via `Game::LookupPlayerColor` and passes it to `BuildStickFigure`. Same for the local inventory preview via `PlayerInventoryPreview`. Default (id=0) is the historical neon green so old clients/servers stay compatible.

To add a new color, append one row to `Game::kPlayerColorTable` in `src/common/entity/PlayerColors.hpp` and bump `PlayerColorId::Count`. The launcher swatch grid auto-includes it.

**Friend hosting without port-forwarding** (added 2026-07): when a player hosts, the game tries to open its port automatically via UPnP IGD (`src/client/network/UPnPPortMapper` — hand-rolled SSDP+SOAP on Asio, no new dependency; runs on a worker thread, failure is non-fatal) and reports the WAN address in its presence. The service then TCP-probes that address to *verify* reachability (`probe_reachable`; a host claiming success isn't enough — double-NAT/ISP filtering). `join_info` returns `mode:"direct"` with the host address when the probe passed, otherwise `mode:"relay"` with a ticket: the service pushes `relay_open` to the host, whose FriendsClient dials OUT to the service and hands the resulting socket to `NetworkServer::AdoptConnection` (native-handle transfer between io_contexts); the joiner sets `NetworkClient::SetConnectPreamble` with a `relay_attach` line and connects to the service, which splices the two streams. Relay traffic rides the SAME port as the control protocol (first-line sniff), so only 25570 needs forwarding on the service machine. `--allow-private-hosts` treats private addresses as directly joinable (LAN-only setups and local testing).

**Friends system flow** (added 2026-07): self-hosted backend `tools/friends_server/friends_service.py` (Python 3 stdlib, port 25570, sqlite; run it next to the game server and port-forward TCP 25570). One port, two protocols sniffed by first byte: HTTP POST `/api` (launcher: signup/login/logout/check_name/rename via `src/launcher/net/FriendsServiceClient`) and persistent NDJSON (game: `src/client/network/FriendsClient` — own io thread, app lifetime, hello(token) → server pushes `roster`/`invite` events; friend-op targets travel in `"friend"`, `"id"` is the correlation field). Launcher login stores `session_token`/`account_id`/`account_name` in `launcher.json`, forces the username to the account name, shows a debounced availability checkmark (rename commits via the service; friendships key on account id so renames propagate). Launch passes `--session`/`--account-id` → PlatformMain constructs `Client::g_friendsClient` (null = guest). Presence transitions live in PlatformMain only: Menu (title) / Hosting(worldName, 25565) / Playing(addr). Friends UI: `screens/FriendsScreen` (title screen's old Realms slot + pause menu). Join posts a `Multiplayer` TitleAction; from in-game the pause branch stashes it in `pendingSessionAction` and the outer session loop auto-joins without showing the title. Transport is plaintext by design (personal scale); the hosting machine's launcher should set `friends_service` to `127.0.0.1` (NAT hairpin).

### MSVC compatibility fixes (Windows build)
The game uses GCC/Clang-specific features that need MSVC equivalents:

## Profiling with Tracy

Tracy is the profiler of record for this project. Currently **v0.14.0** (client pinned in
CMakeLists via FetchContent). Everything below was verified against the actual
fetched source in `cmake-build-tracy/_deps/tracy-src`, not from memory — re-verify
against `NEWS` and the client sources after any version bump.

### Setup invariants (all three have bitten us)

1. **Client and viewer versions must match exactly.** Tracy checks a protocol
   version on connect; a mismatch gives a "Protocol mismatch" dialog and no data.
   The `GIT_TAG` in CMakeLists and the `tracy-profiler` app must move together.
2. **`TRACY_ENABLE` must be set as a CACHE var before `FetchContent_MakeAvailable`.**
   As of 0.14 Tracy's own default flipped to OFF
   (`set_option(TRACY_ENABLE "Enable profiling" OFF TracyClient)`). Setting it only via
   `target_compile_definitions` on `MyVoxelGame` compiles profiling OUT of the
   `TracyClient` library itself. 0.14 added macro-mismatch detection that makes this a
   link error rather than a silently dead profiler.
3. **A stale `libTracyClient.a` survives a `GIT_TAG` bump.** FetchContent re-fetches the
   source but will happily relink the old archive. If a version bump doesn't take:
   `rm -f cmake-build-tracy/_deps/tracy-build/libTracyClient.a`, or
   `rm -rf cmake-build-tracy/_deps/tracy-*` and reconfigure.

**Which build dir has Tracy:** `cmake-build-tracy` (and `cmake-build-debug`).
`cmake-build-release` and `cmake-build-universal` have **no Tracy at all** — a binary
from either will never connect. Check before debugging a "won't connect" report.

### Which tool for which symptom

| Question | Tool |
|---|---|
| Why was *this* frame slow? | Timeline + Zone info (use **Parent zones** — "Zone trace" was removed in 0.14) |
| Is this zone usually slow, or was that a fluke? | **Find Zone** — histogram + distribution across all instances |
| Where does time go overall? | **Statistics**, **Flame graph** (zooms/pans as of 0.14) |
| Did my fix work? | **Compare Traces** — load before/after; much improved in 0.14 |
| What is this counter doing? | Plots |
| Which source line / instruction? | **Sampling** + **Symbol view** |
| Who is blocked on whom? | **Wait stacks** |
| CPU or GPU bound? | GPU zones; also `g_enableGpuPassTimers` (ChunkRenderer.cpp), but it costs ~2.3 ms/call on Apple's GL driver — read it, then turn it off |
| Which part of the session was this? | **Sections** (new in 0.14) — mark world-load vs steady-state, then range-limit stats |

### macOS-specific limits — important when diagnosing stalls

Tracy on macOS has **no context-switch capture**. It cannot tell you whether a thread was
*executing* or *descheduled*. Do not claim starvation from Tracy data alone — cross-check
CPU-usage plots and use `sample <pid> 10 1 -file <out>` (or Instruments) for the real
blocked stack.

**Apple system tracing (0.14, prototype)** — verified in `public/client/apple/TracyMach.cpp`:

- It is **sampling only** (`QueueType::CallstackSample` at 1000 Hz default). It does
  **not** emit context switches.
- `SysTraceStart` gates on `geteuid() == 0` — stock Tracy needs **the GAME** run as root
  (not the viewer). No entitlement or TCC permission exists to grant instead.
- **We patch that check out.** `cmake/PatchTracyAppleSampling.cmake`, wired as a
  `PATCH_COMMAND` on the Tracy `FetchContent_Declare`, so Apple sampling works with no
  `sudo`. Legitimate because upstream's own comment calls the privilege check
  *"technically unnecessary"* — it is user-mode self-sampling via `mach_task_self()`.
  The script is **idempotent** (PATCH_COMMAND re-runs on reconfigure) and warns rather
  than fails if a version bump moves the guard — so after a Tracy upgrade, check the
  configure output for that warning, or you silently lose sampling-without-root.
- **Why not just sudo:** running the game as root creates root-owned files in
  `~/Library/Application Support/obeycraft/` (saves, options.txt, worlds.json), which
  breaks every later normal run. If you ever do run under sudo, follow it with
  `sudo chown -R obey:staff` on that directory.
- **It is OFF by default** — `option(TRACY_APPLE_SAMPLING)` in CMakeLists, which when
  OFF puts `TRACY_NO_SYSTEM_TRACING` on the `TracyClient` target and compiles the whole
  path away. Turning it off does **not** need a re-fetch (the source stays patched, the
  code just compiles out). Safe to set on TracyClient alone, unlike `TRACY_ENABLE`: this
  macro is internal to `TracySysTrace.hpp` and is not macro-mismatch checked. There is no
  runtime off-switch on macOS — the Apple path reads no env var (`TRACY_NO_SAMPLING` is
  honoured only on the Linux path), so CMake is the only knob.
- Sampling suspends each thread per sample, which perturbs the timings being measured.
  **Never compare a sampled capture against an unsampled one.**
- 0.14 colours sample markers: **green** = your program, **blue** = external, **red** =
  kernel. Red inside a stalled zone means blocked in a syscall.

**Ghost zones — the trap that follows from enabling sampling.** Symptom: the timeline
fills with unnamed blocks whose tooltip reads *"👻 Ghost zone / Unknown frame: 0x…"* and
the instrumented zones appear to be gone. Nothing is broken; three facts compound:

1. The viewer's **"Draw ghost zones" option defaults to ON** (`TracyViewData.hpp`,
   `uint8_t ghostZones = true`). Before sampling worked on macOS it was inert, because
   `AreGhostZonesReady()` was always false — there was never any sample data.
2. `TracyTimelineItemThread.cpp` renders ghosts when
   `AreGhostZonesReady() && ( m_ghost || ( vd.ghostZones && thread->timeline.empty() ) )`.
   Ghosts **replace** instrumented zones — any thread with no zones of its own is now
   full of them, and a clickable 👻 icon appears immediately right of *every* thread
   label that has both. That is exactly where you click to expand a thread, so one stray
   click silently swaps a thread's real zones for ghosts (`m_ghost` is per-thread).
3. Frames read "Unknown frame" because macOS is `TRACY_HAS_CALLSTACK 4` → **`dladdr`**,
   which only resolves *exported* symbols. Our binary is not stripped (~121k symbols) but
   they are local, so most game frames never resolve.

**Fix:** Options → uncheck **👻 Draw ghost zones**, or click the 👻 beside the thread name
to flip that one thread back. To make ghost frames actually readable you would need
`-Wl,-export_dynamic`; not worth it — use `sample <pid>` or Instruments instead, which
read the real symbol table.

Also new in 0.14 and worth knowing: several sampling statistics were previously **wrong**
(inclusive counts double-counted inline functions; context-switch samples leaked into
per-symbol lists on load), so do not compare pre-0.14 traces against newer ones.

## Architecture Overview

MyVoxelGame is a Minecraft-compatible voxel engine with a client-server architecture designed for both single-player (integrated server) and multiplayer support.

#### Minecraft Compatibility
- Loads existing Minecraft Java Edition worlds (1.18+)
- Supports Anvil region format (.mca files)
- Compatible block models and texture atlas system
- NBT data structure parsing

### Build Requirements
- CMake 3.19+
- C++20 compiler
- Platform: macOS (universal binary), Windows, Linux

### Dependencies
All external dependencies are vendored in `ext/`:
- GLFW (windowing)
- GLAD (OpenGL loader)
- GLM (math)
- ImGui (debug UI)
- zlib (compression)
- OpenAL (audio)
- STB Image (texture loading)
- nlohmann/json (via FetchContent)
- Boost.Asio (networking, header-only)

### Windows/MSVC portability (things Clang accepts and MSVC does not)

Code written on the Mac builds clean under Clang and then fails on Windows in
three recurring ways. Check these before blaming the Windows toolchain:

- **Transitive includes.** libc++ pulls in `<stdexcept>`, `<string>`, `<deque>`
  and friends through other headers; MSVC's STL does not. Include what you use.
- **Zero-length arrays.** `static const T k_foo[] = {};` is a Clang extension —
  MSVC gives C2466. The generators emit `nullptr` + count 0 instead (see
  `tools/gen_mob_loot.py`).
- **`class` vs `struct` in forward declarations.** MSVC mangles the two tags
  differently (`V` vs `U`), so a `class Foo;` forward declaration of a `struct
  Foo` links fine on Clang and gives LNK2019 on MSVC, pointing at a function
  whose definition plainly exists. Match the tag to the definition.

## ObeyCraft Launcher & Distribution

### Automatic Release System (Zero Manual Steps)

Building in the right configuration automatically handles everything:

| Build | What happens automatically |
|-------|--------------------------|
| Game **Debug** | Normal build, nothing extra |
| Game **Release** (no `-DJALIN=ON`) | Normal build, nothing extra |
| Game **Release** (`-DJALIN=ON`) | Version bumps, zips, uploads to GitHub, uploads debug symbols to Sentry |
| Launcher **Debug** | Normal build, nothing extra |
| Launcher **Release** (no `-DJALIN=ON`) | Version bumps, zips, uploads to GitHub |

**Just build in CLion and everything happens.** No scripts to run.

> **Important:** `-DJALIN=ON` is required for game GitHub uploads and Sentry symbol uploads. Without it, Release builds compile normally but don't publish anything. The launcher uploads on any Release build regardless of JALIN.

### How the Auto-Release Works

1. **Version bump**: Build number files (`tools/game_build_number`, `tools/launcher_build_number`) store an integer that auto-increments on each qualifying build. A generated header (`GameBuildVersion.hpp` / `LauncherBuildVersion.hpp`) is created with the version string.
2. **Compile**: The binary picks up the new version from the generated header.
3. **Post-build**: The app is zipped (stays in the build dir, e.g., `cmake-build-universal/bin/`) and uploaded to GitHub via `gh` CLI. Non-fatal — if offline or `gh` isn't authenticated, the build still succeeds.

Version format: `{major}.{minor}.{build_number}` — game uses `0.1.X`, launcher uses `1.0.X`.

### Player-side diagnostics (log file + crash reports)

Two artifacts land in the obeycraft directory (`~/Library/Application Support/obeycraft`
on macOS). Ask a player for these first — they exist whether or not Sentry got through.

| Path | What it is |
|---|---|
| `logs/latest.log` | Current session. Every `Log::` line, timestamped, no ANSI codes |
| `logs/<date>.log` | Previous sessions, newest 5 kept |
| `crash-reports/crash-<date>.txt` | Version, signal + fault address, backtrace, last ~512 log lines |

**Why this exists alongside Sentry:** the game only ever logged to stdout, which a
launcher-started build discards outright — macOS `open --args` gives the process no
terminal. So a player's "it just closed" came with nothing at all. Sentry is still the
better report when it arrives (symbolicated, aggregated), but it needs network, a live
crashpad process, and a crash of a kind crashpad claims.

**The log file is the primary artifact; the crash report is a bonus.** On macOS Sentry
runs crashpad out-of-process via Mach exception ports, which are delivered *before*
POSIX signals — so for a hard SIGSEGV crashpad can take the exception and the process
dies without our signal handler ever running. The log is written as the game runs, so
it survives regardless. The handler still earns its keep for `std::terminate` (an
uncaught C++ exception is not a Mach exception, so this always fires) and for signals
crashpad doesn't claim.

**`--- log closed cleanly ---` is the tell.** `Log::CloseLogFile` writes it on the
normal exit path only, so a log ending without it means the process died — which is the
one question a crash report can't answer if it never ran.

Implementation notes that are easy to break:
- `CrashHandler.cpp` preallocates the report path, banner and scratch buffer at install
  time and uses only `open`/`write`/`close` + `backtrace_symbols_fd` in the handler.
  `malloc`, `snprintf` and stdio are **not** async-signal-safe, and a crash inside the
  allocator (likely, since heap corruption often *is* the crash) would deadlock on the
  way to writing the report. `backtrace_symbols` allocates; the `_fd` variant does not.
- `Log`'s ring buffer is a fixed `char[512][256]` with an atomic write index for the
  same reason — a `std::deque` behind a mutex is unreadable from a signal handler.
- Install order matters: `InstallCrashHandler` runs **after** `sentry_init`, and chains
  to the previously-installed handler, so Sentry still reports.
- Test with `--crash-test` (SIGSEGV after 3s); it logs the expected report path first.

### Sentry Debug Symbols (Game Only)

On Universal Release builds (with `-DJALIN=ON`, which is the default for `cmake-build-universal`):
1. `dsymutil` generates a `.dSYM` bundle from the game binary
2. The binary is stripped of debug symbols (smaller download for users)
3. `sentry-cli debug-files upload` sends the dSYM to Sentry for crash symbolication
4. Requires `sentry-cli` to be installed (`brew install getsentry/tools/sentry-cli`)
5. Sentry release string uses the auto-incremented version: `myvoxelgame@0.1.X`

### GitHub Release Tag Convention

- **Game releases**: `v0.1.1`, `v0.1.2`, ... (auto-created on Universal Release build)
- **Launcher releases**: `launcher-v1.0.1`, `launcher-v1.0.2`, ... (auto-created on Release build)
- Both coexist in the same repo: `ObiJello/MyVoxelGame-Download`
- The launcher knows which is which by the `launcher-v` prefix

### How the Launcher Update System Works

- **GitHub repo**: `ObiJello/MyVoxelGame-Download`
- **Game updates**: Launcher queries `/releases` and picks the latest tag NOT prefixed with `launcher-v`
- **Launcher self-updates**: Launcher queries `/releases` and picks the latest `launcher-v*` tag, compares against its compiled-in version, silently downloads/installs, shows "Restart to update launcher"
- **Version tracking**: `~/Library/Application Support/obeycraft/launcher.json`
- **Game install location**: `~/Library/Application Support/obeycraft/game/`
- **Asset name matching**: Zip filenames must contain a platform tag (`macos-universal`, `macos-arm64`, `windows-x64`) for the launcher to pick the right one

### Creating a DMG for First-Time Distribution (macOS)
```bash
./tools/create_dmg.sh    # Creates ~/Downloads/ObeyCraftLauncher.dmg
```
This is only needed once to distribute the launcher to new users. After that, the launcher updates itself.

### Creating a Windows Installer
```powershell
# Requires Inno Setup 6 installed at %LOCALAPPDATA%\Programs\Inno Setup 6\
powershell -ExecutionPolicy Bypass -File tools/create_installer.ps1
# Output: %USERPROFILE%\Downloads\ObeyCraftLauncherInstaller.exe
```
Run this whenever you need to do a fresh Windows install (e.g. to bypass a broken auto-update). The `.iss` script is at `tools/create_installer.iss`.

### Manual Release Scripts (Optional)
These still exist if you ever need manual control:
```bash
./tools/release_launcher.sh          # Bump patch, rebuild, upload
./tools/release_launcher.sh minor    # Bump minor
./tools/release_game.sh              # Same for game
```

### Key Files
- **Launcher source**: `src/launcher/` — config in `LauncherConfig.hpp`
- **Build numbers**: `tools/game_build_number`, `tools/launcher_build_number`
- **Auto-release scripts**: `tools/bump_version.sh`, `tools/auto_release.sh`, `tools/update_plist_version.sh`
- **Launcher app icon**: `assets/launcher/logo.png` (converted to `AppIcon.icns` via `iconutil`)
- **DMG builder**: `tools/create_dmg.sh`