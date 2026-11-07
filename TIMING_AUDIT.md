# Timing reference

This document describes how Killer Is Dead's timing breaks above 30 FPS, what KIDFrameRateFix changes, and what it deliberately leaves alone. It covers the supported Steam executable (`KidGame.exe`, timestamp `0x53548489`).

## Principles

The game mixes two kinds of timing. Most movement, timers and animation use `DeltaTime` and already work at any frame rate. A minority of code assumes one update every 1/30 s: it counts frames, applies a constant per call, or advances UI movies by a fixed 1/30 s. Only that second group is changed.

Frame-counted code is brought back to its 30 FPS behaviour in real time. Code that was already time-based is not modified, and the game is never slowed down, time-dilated or locked to a fixed tick. Global fixes of that kind would break the time-based majority of the game.

A textual hit for `Tick`, `Frame` or a numeric constant is not evidence of a bug. Array indices, spatial offsets, angle wrapping, one-shot state transitions and functions that use elapsed time indirectly (`mCurrentDeltaTime`, `IncRtn`) all look similar in decompiled script. Every patched site was confirmed by its behaviour, not by pattern alone.

The candidate list comes from 13,536 functions in the retail `KidGame.u`, `KidGameContent.u` and `GHMEngine.u`, of which 1,359 reference a `DeltaTime`-style operand. `tools/FindDeltaTimeCandidates.ps1` reproduces that filter.

## Mechanisms

### Hooks

| Hook | Purpose |
| --- | --- |
| `FGFxEngine::Tick` | Runs once per rendered frame with the real frame time. It drives Scaleform movie advance, the framerate unlock and limiter, and the fade size. |
| `UKidGFxMgr::Tick` | Drives the UnrealScript HUD, either at 30 Hz or every frame (smooth UI). It also advances the shared 30 Hz generation clock. |
| `FFrame::FFrame` (`+0x356E70`) | Runs when each script function is entered. It discovers target functions on their first call and applies return-code gates. |
| `UObject::ProcessInternal` (`+0x362140`) | Runs at the start of every interpreted function body, after arguments have been copied into `Locals`. It handles everything that reads or writes parameters or bytecode at call time. |
| `UParticleModuleCollision::Update` (`+0x11DADE0`) | Ribbon-emitter collision completion. |

`FFrame` construction happens *before* argument copy in both `UObject::CallFunction` and `UObject::ProcessEvent`. Parameters are therefore read and written only from `ProcessInternal`.

### The 30 Hz generation clock

An elapsed-time accumulator in the HUD tick increments a shared generation counter once per 1/30 s of real time. Every frame-counted patch is keyed to this counter. A gated function runs on its first call in each generation, so it behaves as it did at 30 FPS whatever the render rate. During long hitches, catch-up is bounded by `MaxCatchUpTicks`.

### Patch techniques

| Technique | How it works | Used for |
| --- | --- | --- |
| Return-code gate | On calls after the first in a generation, the frame's code pointer is set to the function's own `return; EndOfScript` tail, so the body is skipped. | Kismet latent waits, unlock notices, sound feedback, smooth-UI helpers |
| Fixed step | As the return-code gate, but each permitted call receives the game `DeltaTime` accumulated since the previous permitted call. | Boss state functions that mix frame counters with time-based work |
| Operand rescale | A literal in the bytecode is rewritten from the real frame time just before the function runs. Every write keeps 30 FPS behaviour unchanged. | Per-call constants and frame thresholds |
| Counter-statement gate | `counter++`/`counter--` statements become an `EX_Jump` over themselves on non-step calls and are restored on step calls. The rest of the body runs every frame. | Smooth-UI functions that also contain frame counters |

### Safety rules

- **Validation.** Every native patch checks the executable timestamp, image size and a unique code signature before it is installed. Every script patch checks the live bytecode: the operand values, the number of matches and the expected return tail. A patch that fails validation stays off, and the log says why.
- **Function lifetime.** Content packages load and unload with levels, so a `UFunction` and its `Script` array can be freed and their address reused. Bytecode writes happen only while the target function is executing. Before every write or redirect, the mod confirms that the function still owns the `Script` array recorded at discovery. When a class reloads at a new address, its patches re-arm.
- **Overhead.** Every armed function is recorded in one hash set, and both script hooks pass any other call straight to the engine after a single lookup. First-call detection uses an open-addressing set keyed on the function and its `Script` pointer, so each function is inspected once per load.

## Fixes

### Framerate and display

| Site | Retail behaviour | Fix |
| --- | --- | --- |
| `UEngine::GetMaxTickRate` | Framerate smoothing caps the game at `MaxSmoothedFrameRate` (31 in the stock INI) | `UnlockFrameRate` clears `bSmoothFrameRate` on the live engine. Offsets come from the engine's reflection data. |
| `UGameEngine::GetMaxTickRate` (`+0x1AC6B70`) | With smoothing off and `GIsGame` set, it still returns `clamp(1/DeltaTime, MinSmoothedFrameRate, MaxSmoothedFrameRate)`, and the engine loop sleeps to that rate | `UnlockFrameRate` sets `MaxSmoothedFrameRate` to 0, which makes the clamp return 0 (unlimited) |
| Startup resolution | `[SystemSettings] ResX/ResY` defaults to 1280x720, and the options menu offers at most 1920x1080 | `Resolution` appends `ResX=`/`ResY=` (desktop or configured size) to the process command line before `WinMain` reads it. UE3 gives command-line values priority over the INI, and values already on the command line are left alone. |
| `ULocalPlayer::CalcSceneView` projection (`+0xCBF223`) | With `AspectRatioAxisConstraint=AspectRatio_MaintainXFOV` (stock) the half FOV is horizontal and the vertical FOV shrinks on wider screens (Vert-). `AspectRatio_MaintainYFOV` treats every FOV as vertical, so it widens 16:9 too and isn't a Hor+ fix | A hook on the store of the half angle rewrites it as `atan(tan(half) * scale)`. `FOVMode=hor+` multiplies `scale` by `max(1, aspect / (16/9))`, which holds the vertical FOV at its 16:9 value. `FOV` multiplies it by `tan(FOV/2) / tan(30°)` unless the view target is a `CameraActor`, so zooms keep their ratios and authored shots keep their framing. Letterboxed cameras take a separate branch and aren't touched. A non-stock constraint in `KidEngine.ini` disables the hook. |
| Screen percentage (`FSystemSettings` `+0xB2EBF0`, `+0xB33010`, `+0xB33120`; `FSceneRenderTargets::Allocate` `+0x157C920`; `FSceneRenderer` view setup `+0x1552D80`; scene colour clear `+0x154F684`) | `ScreenPercentage` only takes effect below 100. `NeedsUpscale` requires it, `ScaleScreenCoords` and `UnScaleScreenCoords` clamp the fraction to 1 and centre the reduced view, the scene buffers are sized to the viewport, the renderer clamps each view's buffer region to the render target size, and the frame's scene colour clear uses the render target size as its viewport | `Supersampling=N` is a per-axis factor (N² samples per pixel, so 2 is 4x SSAA). It makes `NeedsUpscale` true, scales the view rectangle by N from the buffer origin, asks `Allocate` for N times the size (at most 16384 per edge), removes the two render-target clamps, and scales the clear's viewport by N. `UnScaleScreenCoords` maps back to viewport size only for the final pass (identified by its call site); its other callers clear the buffers around the views and keep the full scaled rectangle. The engine's own final pass then filters the scene down to the viewport, after which the HUD draws at native resolution. `ScreenPercentage` is ignored while this is on. Every site is matched by signature, and none is patched unless all are found. |
| Cinematic fade `DrawTile` (`+0x2193173`) | Drawn at a fixed 1920x1080 from a shared `.rdata` pair | The two size operands read mod-owned floats that track the game window's client area |
| Frame limiter | The engine's own limiter paces with coarse sleeps | `FrameRateLimit` paces with a high-resolution waitable timer and a short spin |

### UI

| Site | Retail behaviour | Fix |
| --- | --- | --- |
| `FGFxEngine::Tick` movie advance | Every game-timed Scaleform movie advances by `1/30` s per rendered frame | `Movie::Advance` and `PostAdvance` receive the real frame time |
| `UKidGFxMgr::Tick` | Frame-counted HUD scripts run once per rendered frame | Smooth UI (default): runs every frame with real `DeltaTime`; see *Smooth UI*. Bridge mode: runs at 30 Hz from an accumulator. |
| `KidNewMessage.Tick` | Six-frame input and display guards run per rendered frame, so one press dismisses several unlock notices | One update per generation. Catch-up generations collapse to one update, so a cached input edge can't replay. |

### Gameplay

| Site | Retail behaviour | Fix |
| --- | --- | --- |
| `TickMikaCardiacMassage` | `DownSpeed += ReviveRate` and `MassageSpeed *= 0.9` apply once per frame | Existing constants rewritten to `dt*30` and `0.9^(dt*30)`; opcodes and INI values unchanged |
| Ribbon `ParticleModuleCollisionActor` completion | The 50-contact budget is consumed per update, and `EPCC_Kill` removes particles without repairing ribbon links. This can form a closed ribbon chain that hangs `Tick_RecalculateTangents` (the Mondo Girls 3 cutscene freeze). | The budget is consumed at 30 units/s, and due removals go through the emitter's ribbon-aware `KillParticles`. Details are in `audit/MONDO_GIRLS_3_HANG.md`. |
| `KidSeqAct_MissionComplete`, `MissionWave`, `TicketGet`, `WeaponGet` `.Update` | Latent waits count frames (80, 50 and 100) and ignore `DeltaTime` | Return-code gate |
| `OnFinishDrillCancel`, `KidPlayerController.PlayDamageSoundCueAndParticleSystem`, both `PlayLeverSoundSE` | Feedback-only sound helpers called per frame by their callers | Return-code gate. Audio mixing, damage, collision and state are untouched. |
| `KidPickupFactory_Collectable.UpdateAbsorptionPlayer` | Attraction speed `+= 0.5` per call | Operand rescale to `0.5*dt*30` |
| `KidDroppedPickup.UpdateAbsorptionPlayer` | Forced pickup after `mMoveFailCount > 10` failed frames | Operand rescale to `10/(dt*30)`, clamped to the byte operand's 1–255 range. That holds the 333 ms fallback up to 765 FPS, and 255 ms at 1,000 FPS. |
| `KidGiantHeadThrowItem.Tick` | Pitch `+= 500` per call | Operand rescale to `round(500*dt*30)` |
| `UXAudio2Device::Init` (`+0x20CAD00`) | The stock `[XAudio2.XAudio2Device]` settings (`MaxChannels=48`, `CommonAudioPoolSize=0`, `MinCompressedDurationGame=5`) are associated with crashes after boss fights; the standard workaround is an INI edit | `AudioDeviceFix` sets the workaround values on the live device as `Init` starts, before voices and the resident pool are allocated: `MaxChannels` at least 64, `CommonAudioPoolSize` 320 when 0, `MinCompressedDurationGame` at most 0.3. Offsets come from reflection, and `MaxChannels` is checked against the `+0x40` that `Init` itself reads. |
| Cinematic freeze with root motion | Cinematic mode ignores move input but leaves the mesh in `RMM_Accel`, so a playing locomotion animation keeps moving the pawn. After the Scarlett conversation in the Alice key mission, this carries Mondo off the heart lift as it teleports and descends. | `CinematicRootMotionFix` zeroes the translation of `KidPawn.RootMotionExtracted`'s `out` delta, written through `FFrame::OutParms`. It applies only to the player, in cinematic mode with `m_bCinematicIgnoreMoveInput`, in `PlayerWalking`, with `RMM_Accel`. Intentional scripted root motion uses `RMM_Translate` or a dedicated controller state, so it isn't affected. |

### Bosses

| Site | Retail behaviour | Fix |
| --- | --- | --- |
| `KidAIController_GiantHead.StunBackState.Tick` | A one-frame startup wait and a two-frame damage lockout | Fixed step |
| `KidAIController_GiantHead.TracePlayerBeamesState.Tick` | Beam IK weight changes by `0.05` per call | Fixed step |
| `KidAIController_ThomasBase.MotionFunctionTick` | `mMotionFunctioPlayFrameCount` advances per call and drives the sine motion and strike phases | Fixed step |
| `KidAIController_Sebastian.AbsorbState.Tick` | Vertical impulses of 15/25 per call, plus a sine term accumulated into velocity | Fixed step |
| `KidAIController_Sebastian.Tick` called from `StunState` | The flying-height branch sets `Velocity.Z = 0` every call, erasing the small high-FPS gravity steps of falling physics | While `StunState.Tick` is the caller, the branch's state guard is switched so the clamp is skipped. Every other state keeps the retail branch. A 30 Hz schedule of the clamp is not equivalent, because its phase relative to state entry changes landing positions and QTE availability. |

Fixed-step targets must take `DeltaTime` as their first parameter (a `FloatProperty` at frame offset 0); discovery checks this.

## Smooth UI

All Scaleform movies in the game are authored at 30 fps (a few at 31). Timeline animation, baked Flash tweens and ActionScript `onEnterFrame` logic advance in whole SWF frames. They play at the correct speed, but gaining intermediate frames would require interpolating Scaleform's display list, which is out of scope.

The UnrealScript HUD layer runs every frame in smooth-UI mode. Each part is handled according to what it contains:

| Function | Frame-dependent content | Treatment |
| --- | --- | --- |
| `KidGFxMgr.Tick` | Fade `±= FadeSpeed/30` per call; `m_iFadeInDelayTime--`; four `m_iTutorialWindowCounter++` waits | Runs every frame. The fade divisor becomes `1/dt`, and the five counters keep the 30 Hz clock. |
| `KidGFxMgr.EarthControlTick` | Title globe ease `alpha += (1-alpha)*0.1` per call | Runs every frame; `0.1` becomes `1-0.9^(dt*30)`, which is exact |
| `KidGFx_TitleMenuContent.Tick` | Logo `Alpha ±= 1/6` and `Reverse ±= 1/30` per call; six state-machine counters | Runs every frame. The four ramp numerators are scaled by `dt*30`, and the six counters keep the 30 Hz clock. |
| `KidGFxMgr.PauseEventTick`, `KidGFx_MovieStatus`, `MovieResultContent`, `ClearStats`, `Leaderboard`, `ABVision` `.Tick` | One-shot or staged frame delays only | 30 Hz return-code gate |
| `KidGFx_MondoGauge`, `MondoStatus`, `WeaponChange`, `BurstVision`, `ButtonGuide` `.Tick` | Update throttles, shake duration and per-frame shake offsets, visibility delays. Gauge values are pushed into ActionScript, which animates at the SWF frame rate. | 30 Hz return-code gate; running these every frame would change only the shake jitter frequency |

## Framerate limits

With both engine caps removed, the game enforces no further limit. Neither the game thread nor the render thread sleeps or spins on a timer to pace frames. The roughly 140 `QueryPerformanceCounter` calls per frame on the game thread come from the build's stat cycle counters, and render-thread waits are uncontended mutexes.

A frame rate that stops at the same value whatever is on screen comes from outside the game, most commonly the GPU driver's frame pacing. On NVIDIA, G-SYNC with Low Latency Mode set to Ultra caps the frame rate slightly below the display's refresh rate by design. Any type of V-Sync enabled in the driver caps the frame rate at the monitor's refresh rate.

## Verified as time-based

- `KidQTEManager.Tick` and `TickQTEData`: durations and attachment interpolation use `DeltaTime`.
- `KidPawn.RunAttackFrame`: attack windows follow animation time.
- Projectile and actor movement, lifespans and ordinary timers use the tick delta.
- `miGameTotalTickCount` is a same-frame identity used for collision and QTE deduplication, not elapsed time. Converting it to seconds would break deduplication.
- `KidCameraMode_Die.CalcCamera`: the raw `65536` only wraps a completed rotation.
- `KidAIPawn_NormalEm_Backbeard.TickFloatPos`: the raw `1.0` wraps a `DeltaTime`-driven spline parameter.
- Dolly attack states that don't read their `DeltaTime` call `IncRtn`, which uses the controller's `mCurrentDeltaTime`. Their velocity assignments are resets.
- `KidAIController_ThomasEyeBase.StrikesAttackState_Phase1.Tick` adds concussion damage once per explicit strike event, not per frame.
- Coordinate additions in camera, targeting, pickup and projectile code are fixed offsets from a freshly computed base.

## Reviewed and left unchanged

- **`KidPawn.TickNotifyTimer`:** `miWaitFrame` counts frames, but every cooked caller passes 0 except one, which passes 1. The worst-case difference is 33 ms on a path used by every animation notify, which doesn't justify a patch.
- **`KidAIPawn_GiantHead.Tick` and `KidProjectile_GiantHeadEyeBeam.Tick`:** both increment frame counters, but neither has a meaningful script consumer. The pawn counter is unused, and the projectile's modulo branch is empty.
- **`KidAIController_Sebastian.Tick` outside `StunState`:** the `Velocity.Z -= 5` hover correction above `m_fHighStable` is applied per call. It has no confirmed high-FPS symptom, so it stays unchanged.

## Open issues

### Boss damage and launch

Reports of bosses taking repeated damage or launching too far may share one cause: the same attack being applied more than once. `KidPawn` keeps a 30-entry ring of recent attack identities, and the final damage application is native. Legitimate multi-hit attacks exist, so suppressing repeated attacker/attack pairs globally is unsafe. A repair needs telemetry around `KidPawn::AttackDamage` and the history push/check functions for a known hit: attack ID, attacker, victim, time, history result, damage and momentum. That would separate three possible causes:

1. one attack applied twice;
2. two distinct attacks emitted by a frame-dependent state;
3. one application whose magnitude is integrated per frame.

### Voice cutoffs

No cooked script contains a voice-duration counter that advances per frame. Playback is native (audio components and FaceFX), and the scripted lifetime checks use timers, `DeltaTime`, animation time or `IsPlaying`. A high-FPS cutoff would most likely be an early ownership change: a repeated event reusing a shared component, an early state exit tearing down FaceFX, or a latent Kismet action finishing too soon. Native `SeqAct_PlaySound` timing and channel eviction are not visible in script. `audit/AUDIO_VOICE_TIMING_AUDIT.md` ranks the candidates and lists the telemetry needed.

### Sebastian third critical-hit Matinee

The third RT+X critical hit plays `SeqAct_Interp_4` (5.333 s) from `CINE_S03_SebBattle_Matinee`. Its `Completed` output leads through a camera fade to `KidSeqAct_LoadingTips_7`. `SeqAct_Interp` advances natively, so it's undetermined whether reports of this scene ending early come from the Matinee clock or from its tracks. `[Diagnostics] BossFinisherTiming` logs the interval from `KidSeqAct_ToggleCinematicMode_5.Activated` to the loading action:

- about 5,333 ms means the Matinee clock is correct;
- about 5,333 × 30 / FPS ms means one 1/30 s step per rendered frame.

## Diagnostics

`[Diagnostics]` options in `KIDFrameRateFix.ini`, all off by default:

| Option | Output |
| --- | --- |
| `FrameRateReport` | Average, fastest and slowest frame rate every 5 s |
| `TimingProbe` | A 10-second census of every timer and wait call made by `KidGame.exe`, with call sites and threads |
| `ScriptTrace` | Each call to the named UnrealScript functions, with object, state and first arguments |
| `SmoothUIGate` | Extra UI functions to keep on the 30 Hz gate in smooth-UI mode |
| `CrashDump` | A full memory dump on the first access violation in `KidGame.exe` |
| `BossFinisherTiming` | Sebastian stun, landing and critical-hit Matinee timings |
