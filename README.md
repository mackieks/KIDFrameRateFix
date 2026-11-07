# KIDFrameRateFix

Dynamic FPS unlock mod for Killer Is Dead: Nightmare Edition (PC). Works on Steam Deck.

## Overview

Killer Is Dead ships with a 30 FPS cap. You can lift it with a `KidEngine.ini` edit, but much of the game was written assuming 30 updates per second, so things break at higher frame rates. The HUD and menus run several times too fast, unlock notices skip, the cardiac massage minigame becomes nearly impossible, some boss phases misbehave, and one Mondo Girls cutscene hangs the game outright.

This mod removes the cap and fixes those timing bugs, so the game plays the same at 60, 144, 480, or 1000 FPS as it does at 30. Anything that was already time-based is left alone. Only code that counted frames is changed, and it is brought back to its original 30 FPS timing in real time. The game itself is never slowed down or locked to a fixed tick.

It's a single standalone DLL. No ASI loader or INI edits needed.

## Install

1. Download the latest release.
2. Extract `dinput8.dll` and `KIDFrameRateFix.ini` into `Killer Is Dead\Binaries\Win32`, next to `KidGame.exe`.
3. Start the game as usual.

To check that it's working, open `KIDFrameRateFix.log` in the same folder. It's rewritten on every launch. It should contain a `Frame-rate unlock: found KID_GameEngine_0` line, and a line for each fix as it activates. Some fixes only activate when you reach the part of the game they apply to.

## Configure

Edit `KIDFrameRateFix.ini`. The useful settings:

| Setting | Default | |
| --- | --- | --- |
| `FrameRateLimit` | `0` | Frame cap in FPS (30–1000). `0` is uncapped. This limiter is more precise than the game's own. |
| `UnlockFrameRate` | `1` | Removes the 31 FPS cap at runtime. Set to `0` if you'd rather manage it in `KidEngine.ini`. |
| `Resolution` | `auto` | Starting resolution. `auto` uses your desktop resolution, `1920x1080` (or any size) forces that, `0` keeps the game's own setting. `ResX=`/`ResY=` in Steam launch options take priority. |
| `FullscreenFadeFix` | `1` | Sizes the black fade-to-black overlay to your resolution instead of a fixed 1920x1080. |
| `FOVMode` | `hor+` | `hor+` shows more at the sides on screens wider than 16:9. `vert-` is the game's own behaviour, which crops the top and bottom instead. No difference at 16:9. |
| `FOV` | `60` | Gameplay field of view in degrees, measured horizontally at 16:9. 60 is stock. Aiming, lock-on and battle zooms scale along with it; cutscene camera shots don't. |
| `SmoothUIExperimental` | `1` | Runs the HUD scripts every frame, so their fades and the title screen animations follow your frame rate. `0` runs them at 30 Hz. |
| `Enabled` | `1` | `0` turns the whole mod off without removing it. |

Every individual fix also has its own on/off switch under `[Gameplay]`. Leave them on unless you're tracking down a problem.

## Uninstall

Delete `dinput8.dll` and `KIDFrameRateFix.ini`.

## Compatibility

- **Game version:** the current Steam release only (`KidGame.exe` timestamp `0x53548489`). Every patch checks the executable and the live game code before it writes anything. On a mismatch, that patch stays off and the log says why.
- **ASI loader mods:** you can keep using an ASI loader alongside this. Alternatively, rename `dinput8.dll` to `KIDFrameRateFix.asi` and load it through the ASI loader instead. If both copies are installed, only the first one loaded does anything.
- **SUWSF ultrawide fix:** the fade fix here replaces SUWSF's "CutscenesFadeout" patches. If you still use SUWSF, turn those two patches off (or drop SUWSF entirely if that's all you used it for).
- **Other mods named `dinput8.dll`:** only one can be installed at a time. Use the `.asi` route above.

## What it fixes

- **Frame-rate cap.** The game has two caps: frame-rate smoothing (31 FPS stock), and a second clamp to `MaxSmoothedFrameRate` that stays active even with smoothing off. The mod removes both at startup, so `KidEngine.ini` doesn't need editing. If your frame rate tops out a little below your monitor's refresh rate, that's most likely G-SYNC with Ultra Low Latency Mode, which caps the frame rate on purpose. It's not the mod.
- **HUD and menus.** Scaleform advanced every movie by 1/30 s per rendered frame, so the UI ran at your frame rate divided by 30 (about 8x at 240 FPS). Movies now advance by the real frame time. The HUD scripts behind them keep their original 30 Hz counters.
- **Unlock notices.** One button press no longer dismisses several notices at once.
- **Cardiac massage.** Mika's CPR revive minigame plays the same as at 30 FPS, with its stock settings. If you changed its values as a workaround, put them back.
- **Mondo Girls 3 hang.** A ribbon-particle collision bug in the engine could create a looping particle chain and freeze the garden cutscene. Collision kills now go through the engine's ribbon-safe path.
- **Mission and reward sequences.** Mission complete, wave, ticket and weapon-get sequences use their intended durations.
- **Bosses.** Fixes Giant Head's stun and beam phases, Thomas's motion, Sebastian's absorb phase, and Sebastian staying airborne after being stunned.
- **Repeated sounds.** The zero-blood drill cancel, player-hit feedback and Gigolo lever QTE sounds no longer fire every frame.
- **Pickups.** Collectibles no longer fly to you 14x too fast, and stuck enemy drops no longer snap to you instantly.
- **Giant Head's thrown objects.** They no longer spin 14x too fast.
- **Crash after boss fights.** The mod applies PCGamingWiki's audio-device workaround (more audio channels, a resident audio pool, and streaming decompression for longer sounds) to the game's audio device at startup, instead of you editing `KidEngine.ini`. It never lowers values you've already raised.
- **Resolution.** The game starts at 1280x720 and its options menu stops at 1920x1080. The mod starts it at your desktop resolution instead, with no `KidEngine.ini` edit.
- **Ultrawide and FOV.** The game is Vert-, so ultrawide screens lose picture at the top and bottom. The mod makes it Hor+ and adds an FOV setting. Don't combine this with another widescreen FOV fix, or the two will stack.
- **Fade to black.** The cinematic fade covers the whole screen at any resolution, not just 1080p.
- **Alice mission heart platform.** After you talk to Scarlett, stepping back onto the heart lift used to shove Mondo onto the upper ledge, and the lift went down without him. This is an engine-level bug: the game freezes the player for the ride, but a walk animation that's still playing keeps moving him through root motion. The fix holds that leftover animation movement whenever a cutscene trigger has frozen the player, so it should catch any other spot in the game with the same problem.

[TIMING_AUDIT.md](TIMING_AUDIT.md) has the details for each fix: what the game does, why it breaks, and exactly what the mod changes.

## What it doesn't fix

- Animation drawn inside the Flash UI movies is authored at 30 fps, so menu slides and gauge animations still update 30 times a second. They play at the correct speed, just not with extra frames in between.
- Prerendered videos are 30 fps.
- I haven't been able to reproduce two reported issues: bosses taking double damage or being launched too far, and voice lines cutting off early. The notes for both are in [TIMING_AUDIT.md](TIMING_AUDIT.md#boss-damage-and-launch) and [audit/AUDIO_VOICE_TIMING_AUDIT.md](audit/AUDIO_VOICE_TIMING_AUDIT.md). If you can reliably reproduce either, please open an issue.

## Building

You'll need Visual Studio 2019 or 2022 with the C++ x86 tools. Then run:

```bat
build.cmd
```

That produces `build\dinput8.dll`. Run `build.cmd install` to also copy it into `..\Binaries\Win32` if the repo sits inside the game folder.

`tools/` has the research tools used to build this:

- `KidInspect.cpp`: an inspector that attaches to a running game. It lists objects, reads properties, dumps function bytecode, and records the player's movement and state. Its `poke` command writes memory, with the game suspended, for live A/B tests.
- `ThreadSampler.cpp`: a sampling profiler that shows where each of the game's threads spends its time.
- `DumpKismet.py` and `KismetGraph.py`: read a level's Kismet scripting and trace it forwards or backwards.
- The package decompression and dump tools used for the audit.

The game files they read aren't included.

## License

MIT, see [LICENSE](LICENSE). Contains no game code or assets. Killer Is Dead is © Kadokawa Games / Grasshopper Manufacture.
