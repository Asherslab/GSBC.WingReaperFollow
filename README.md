# WING Follow for REAPER

A native REAPER extension (Windows + macOS) that makes REAPER tracks **follow a Behringer WING**:
fader and/or mute, per track, with global on/off switches. It's built for a livestream mix that
runs in REAPER from the WING's Dante feed. On nights with nobody on the stream desk, the stream
follows what FOH does.

It is **strictly one-way**. The extension only ever *reads* from the console, and its OSC encoder
cannot even build a message that carries a value (see `src/wing/osc.h`).

## What you get inside REAPER

- **Dockable window** (*Extensions → WING Follow → Show window*, or the action
  *WING Follow: Show/hide window*). It has connection settings, the global switches, and the
  mapping list with live WING values and status.
- **Global quick switches**, also available as actions for toolbars (buttons light up when on):
  - *Toggle following the WING (master switch)*, plus separate *Start* and *Stop* actions
  - *Toggle following mutes (all mappings)*
  - *Toggle following faders (all mappings)*
  - *Enable all mappings* / *Disable all mappings*
  - *Toggle connection to the WING*, *Re-sync from the WING now*
- **Track and mixer right-click menu**, under *WING Follow*: map the selected tracks by track
  number, toggle following for them, or add the strip FX.
- **Optional mixer-strip FX** ("WING Follow (mixer strip)", a JSFX that the extension installs by
  itself). It shows the mapping *inside the mixer strip*, e.g. `OUT CRD.12` → `CH 5`, with
  ON / M / F buttons and a colour bar for status. Click the label to change what the track
  follows. It is pure pass-through audio. Right-click it in the mixer and enable
  **Show embedded UI** to see it in the strip.

## What a track can follow

Faders and mutes exist only on WING *strips*. A mapping can still be defined in three ways:

| Kind | Example | Follows |
|---|---|---|
| **Output** (recommended) | `OUT CRD.12` | Whatever feeds card (Dante) output 12. If that is an input source, the channel currently using that source. If it is a bus, main or matrix, that strip. If it is a user signal, whatever that user signal taps. |
| **Input source** | `IN A.5` | The channel currently using source A.5 as its input (main or alt) |
| **Strip** | `CH 3`, `DCA 2`, `BUS 1`, `MGRP 4` | That strip directly |

The WING separates *sources* from *channels*, and REAPER receives sources over Dante. So an
**Output** mapping keeps working when FOH re-patches: "REAPER track 12 = Dante channel 12" always
follows whichever channel that signal currently lands on. Stereo sources (mode ST/MS) are handled.
If several channels use the same source, the lowest-numbered channel is followed, and the window
shows "(+N more)".

**Several mappings on one track combine the way the console does.** Fader dB values add up, and
the track is muted if *any* of them is muted. Map `OUT CRD.5` **and** `DCA 2` to the vocal track,
and the stream follows both the vocal channel and the vocal DCA, including the DCA mute. Mute
groups work the same way (`MGRP n`, mute only).

Every mapping has its own **On**, **M** (follow mute) and **F** (follow fader) switches and a
**dB offset**, for example to sit a channel 3 dB hotter on the stream than in the room.

A value is applied only when it is *allowed* (master on, the global switch on, the mapping on) and
*known* (it has arrived from the console). When the connection drops, REAPER keeps its last
values; it never snaps to silence. REAPER is only written when the WING value changes. So if you
nudge a stream fader by hand, it stays where you put it until FOH moves that fader.
Turning on the master switch, reconnecting, or pressing *Re-sync* re-asserts everything.

## First-time setup

1. Install the extension from the latest
   [GitHub release](../../releases/latest). **Quit REAPER first.**
   - **Windows:** run `WING-Follow-<version>-Windows-Setup.exe`. It installs for your user into
     `%APPDATA%\REAPER\UserPlugins` (no admin needed). For a portable REAPER, pick that
     install's `UserPlugins` folder instead. It can be uninstalled from *Apps & features*.
   - **macOS:** open `WING-Follow-<version>-macOS.pkg`. It installs for your user into
     `~/Library/Application Support/REAPER/UserPlugins`. The package isn't notarized, so the first
     time macOS may block it: right-click and choose **Open**, or allow it under
     *System Settings → Privacy & Security*.
   - Manual install: copy the bare `.dll` / `.dylib` from the release into `UserPlugins`. A
     downloaded `.dylib` may need `xattr -d com.apple.quarantine reaper_wingfollow.dylib`.
2. *Extensions → WING Follow → Show window*. Click **Find...** (or type the WING's IP), then
   **Connect**. The status line shows the console's name once it answers.
3. Select the Dante input tracks. In the **New** row choose `Output` / `CRD`, then click
   **Add sel. tracks by track #**. Track 1 now follows card output 1, track 2 card output 2, and
   so on. Use **Add sel. tracks from #** instead if the numbering is offset.
4. Add DCA and mute-group mappings to the tracks (or folder/bus tracks) that should follow them.
5. Optionally click **Add strip FX** for the selected mappings to get the mixer-strip view.
6. Tick **Follow WING**, or bind *WING Follow: Toggle following the WING* to a toolbar button.

Mappings are saved in the project (keyed by track GUID, so reordering or renaming tracks is
fine). Connection settings and the global switches are saved globally.

## Network notes

- WING OSC is UDP port **2223**; discovery uses "WING?" on UDP 2222. The PC's control network
  must reach the WING's **network (Ethernet) port**. The Dante network alone is not enough.
- **The WING allows only one OSC "subscriber" at a time, and the last one to ask wins.** If
  Bitfocus Companion (or another OSC app) also subscribes, the two will take turns. WING Follow
  therefore also **polls** what it follows (*Poll ms*, default 500). If another app owns the
  subscription, untick **Live updates** and lower *Poll ms* (e.g. 100–200) so the two don't fight.
- **Log OSC** prints the raw traffic to REAPER's console, which is useful the first time on a real desk.

## Things to verify on the real WING

This was built against the WING OSC documentation (Maillot, "WING OSC" v0.3.2 and *WING Remote
Protocols*) and cross-checked with the Bitfocus Companion WING module. It has **not yet been run
against a physical console**. Check the following with **Log OSC** on:

1. The subscription command `/*S` is accepted, and pushed updates use the same `,sff`/`,sfi` form
   as query replies. Companion uses `/*S`; the doc writes `/*s`.
2. Channel input routing: `/ch/N/in/conn/grp|in|altgrp|altin` and `/ch/N/in/set/altsrc`. The global
   `/io/altsw` switch is not used; the per-channel `altsrc` flag is assumed to decide which input
   is active.
3. The group names that appear in `/io/out/<grp>/<n>/grp` and `/io/user/<n>/grp`. `BUS`, `MAIN`,
   `MTX`, `USR` and `CH` are handled; anything else shows "not a followable signal".
4. Stereo sources report `/io/in/<grp>/<n>/mode` as `ST` or `MS`.

And in REAPER (Windows and macOS):

5. The track/mixer right-click submenu appears. The mixer context name
   `Mixer control panel context` is a guess; the track one is confirmed.
6. The strip FX inserts (`JS:WING Follow/wing_follow.jsfx`), its embedded UI draws in the mixer,
   and clicks sync back to the window.

## Building

Requirements: CMake 3.20+, a C++17 compiler (MSVC 2019+ on Windows, Xcode command-line tools on
macOS). CMake downloads the REAPER SDK and WDL (pinned commits) the first time it configures.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release              # macOS: add -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target install_to_reaper   # copies into UserPlugins
```

Cross-compiling the Windows DLL on a Mac (for quick checks; releases use MSVC in CI):
`brew install mingw-w64`, then configure with `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake`.

GitHub Actions (`.github/workflows/build.yml`) builds Windows x64 (MSVC) and macOS universal on
every push, runs the tests, and uploads the installers and binaries as workflow artifacts.

**Releasing:** push a version tag. `release.yml` builds and tests with that version stamped in,
then publishes a GitHub Release with the Windows installer (Inno Setup, `packaging/windows/`), the
macOS `.pkg` (`packaging/macos/build_pkg.sh`) and the bare binaries. A tag with a suffix
(`v1.2.0-rc1`) becomes a pre-release.

```sh
git tag v0.1.0 && git push origin v0.1.0
```

## Testing without the console

```sh
python3 tools/wingsim.py          # a WING simulator on UDP 2223 (+2222 discovery)
```

Point the plugin at `127.0.0.1` and drive it: `fader ch 6 -10`, `mute dca 2 on`,
`patch ch 3 A 5`, `out CRD 12 BUS 3`, `sweep ch 1`, `scene`, `steal on` (another app took the
subscription). The simulator **exits with an error if the plugin ever tries to set a value** on
it. Type `help` for the full list.

Automated tests (`ctest`):
- `test_core` covers the OSC codec, WING value decoding, routing resolution (patch changes, alt
  source, stereo pairs, card→bus, card→user signal→channel), mapping save/load, and the
  fader/mute combining rules.
- `test_client` runs the real network client against an in-process fake WING. It checks the
  initial sync, subscription pushes, polling when the subscription is lost, detecting the console
  dropping out and coming back, and that no value-carrying message is ever sent.

## Layout

```
src/wing/      protocol core, no REAPER dependency (OSC codec, UDP, WING model/routing, client thread)
src/ext/       REAPER extension (engine, dock window, actions/menus, entry point, dialog resource)
src/jsfx/      the mixer-strip JSFX (embedded into the extension, installed into Effects/WING Follow/)
tests/         unit + integration tests
tools/         wingsim.py console simulator
packaging/     Windows installer (Inno Setup) and macOS .pkg scripts
```

## Design notes

- **Threads:** one network thread (socket, subscription renewal, paced polling, discovery). All
  REAPER API calls happen on REAPER's main thread in a ~30 Hz timer. The two share only
  mutex-guarded queues: received values are coalesced per address, so a scene-recall burst
  becomes one apply per track.
- **Applying:** `CSurf_OnVolumeChangeEx` / `CSurf_OnMuteChangeEx` (no gang), so control surfaces
  and automation write modes behave as if a control surface moved the fader. No undo points are
  created. A track whose automation is in *Read* mode with an envelope follows the envelope, not
  the WING.
- **Strip FX sync:** the extension watches linked FX parameters. A click in the strip updates the
  mapping, and an edit in the window updates the strip. Removing an FX that created its mapping
  removes the mapping. Removing one that was added to an existing mapping just unlinks it (so
  undoing *Add strip FX* doesn't lose the mapping). Status is not written to the FX while its
  track is in touch/latch/write mode, so it never records automation.
- Inspired by an earlier X32 attempt (`Asherslab/x32-reaper-mirror`, plan only). Carried over:
  one-way guarantee, GUID-keyed per-project mappings, master switch plus enable/disable-all, and
  control-surface-style apply. New here: WING protocol, source/output-based following,
  console-style combining, and a JSFX (instead of a VST2) for the in-strip UI.
