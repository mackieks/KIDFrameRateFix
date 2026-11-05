# Audio and voice high-frame-rate timing audit

## Conclusion

The cooked script does **not** contain a general voice-lifetime counter that advances once per rendered frame. Voice playback is normally delegated to `AudioComponent`, FaceFX, or native `GHM_SoundManager` code, and most callers either use Unreal timers/`DeltaTime` or wait for `IsPlaying` to become false.

## Confirmed rapid-feedback paths

Four effects do have a different, narrowly scoped high-frame-rate defect: their caller polls an action every rendered frame and executes a feedback-only script function on every failed/repeated result. `KidWeapon_Drill.OnFinishDrillCancel` contains only `GHM_SoundManager.Play(m_DrillStopSe, self)` and is reached repeatedly while a zero-Blood Drill attempt remains held. `KidPlayerController.PlayDamageSoundCueAndParticleSystem` is the shared player-hit feedback path used during rapid repeated hits such as Sebastian's bat swarm. `KidPlayerSpecialMove_Jigoro.PlayLeverSoundSE` and `KidSeqAct_ScarletCutScene.PlayLeverSoundSE` also consist solely of a sound play and are called from their per-frame QTE input loops whenever a lever/mash/rotation pattern is active. None represents an audio-clock or sound-duration bug.

The mod's `SoundRetriggerFix` limits only those four verified feedback functions to one execution per 30 Hz logical generation, preserving their retail cadence while leaving the underlying damage, collision, attack, and audio-manager behavior intact. It intentionally does not rate-limit `GHM_SoundManager` globally.

The most credible failure mode is therefore not the XAudio2 sample clock running fast. It is a voice losing ownership early: gameplay starts another cue on the same component, explicitly stops the component or FaceFX animation, or advances a Kismet/state-machine branch that tears the voice down. Existing high-frame-rate defects can make those calls arrive early or too often even when the audio renderer itself is correct.

There is one important unresolved native boundary: `SeqAct_PlaySound` maintains a `SoundDuration` value for latent Kismet output timing, and the retail implementation of its update is native. The cooked packages expose the property but not the arithmetic. A premature `Finished`/marker output could make map logic stop or replace a voice even though the component's own clock is sound. This needs runtime evidence before patching.

## Playback topology

There are three principal paths:

1. `GHM_PlayerController.Kismet_ClientPlaySound` first attempts `PlayActorFaceFXAnim` when the cue contains FaceFX data. If that fails, it creates an `AudioComponent`, sets it to auto-destroy, and gives it to `GHM_SoundManager.PlayAudioComponent` (`GHMEngine_functions.txt:9570-9604`).
2. Character and mission classes keep persistent components such as `m_TelephoneAC` or `m_AudioComponent`, replace their `SoundCue`, and replay them (`KidGame_functions.txt:127893-127920`, `KidGameContent_functions.txt:76081-76103`).
3. Kismet uses native `SeqAct_PlaySound`/`GHM_SeqAct_PlaySoundMarker` actions. Marker registration, duration tracking, playback, and stop behavior ultimately cross native code that is not represented by the decompiled UnrealScript bodies (`GHMEngine_functions.txt:10791-10859`).

The native manager functions most relevant to playback ownership are also stubs in the cooked view:

- `GHM_SoundManager.IsPlaying` (`GHMEngine_functions.txt:10954`)
- `GHM_SoundManager.PlayAudioComponent` (`GHMEngine_functions.txt:11044`)
- `GHM_SoundManager.StopAudioComponent` (`GHMEngine_functions.txt:11053`)
- `GHM_SoundManager.SetSoundClassToOneSoundPerActor` (`GHMEngine_functions.txt:11242`)
- `AudioDevice.ManageSingleSoundPerActorCases` (`Engine_functions.txt:19244`)

## Ranked candidates

| Rank | Candidate | Evidence in cooked code | How high FPS could cause a cutoff | Confidence |
| --- | --- | --- | --- | --- |
| 1 | Shared component replacement and explicit stop-before-play | `KidPawn_Mondo.PlayGigoroGirlVoice` always calls `StopGigoroGirlVoice`, then overwrites and replays `m_TelephoneAC` (`KidGame_functions.txt:127893-127912`). `KidGigoroGirl_Base.PlayGigoroGirlVoice` similarly stops the old FaceFX/component voice before selecting the next cue (`KidGame_functions.txt:49442-49518`). `KidSeqAct_ScarletCutScene.PlayGigoroGirlVoice` stops and reuses its one component (`KidGameContent_functions.txt:84087-84120`). | A duplicated notify, overly fast state advance, or repeated Kismet activation does not overlap voices; it truncates the first one immediately. The audible symptom can therefore originate in a non-audio frame bug. | High for the cutoff mechanism; medium for the FPS trigger until a specific cue is captured. |
| 2 | FaceFX teardown at an early state/animation boundary | Kismet prefers `PlayActorFaceFXAnim`; `StopActorFaceFXAnim` stops the associated facial playback (`Engine_functions.txt:44663-44681`). `KidGigoroGirl_Base.StopGigoroGirlVoice` explicitly chooses that stop path whenever FaceFX is active (`KidGame_functions.txt:49442-49469`). | If a high-FPS gameplay counter exits a state or restarts an animation early, the FaceFX animation and its voice can end together even though the sound duration is correct. | Medium-high. This is especially likely if affected clips also lose lip sync at the cutoff. |
| 3 | Native latent `SeqAct_PlaySound`/marker completion | The class owns `ExtraDelay`, `SoundDuration`, `BeforeEndTime`, and stopped/delay flags (`Engine_objects.txt:25405-25419`). Its source declaration describes `SoundDuration` as the remaining duration used for latent output timing, but the retail update arithmetic is native. `GHM_SeqAct_PlaySoundMarker` also exposes native marker callbacks. | If the native latent action consumes the wrong timestep or is updated too many times, `BeforeEnd`/`Finished` or a marker can fire early. Map logic may then start another line, stop the current line, or leave the state. This would sound like an audio cutoff while being a sequence-timing defect. | Medium; important but not proven statically. |
| 4 | `IsPlaying` start-observation races | Scarlet's `Tick_AddFirstTime`, `Tick_AddChallenge`, and `Tick_Second` play a voice in one switch case and can immediately fall through to a case that advances when `IsPlayVoice` is false (`KidGameContent_functions.txt:75469-75779`). Its query is cue-wide `GHM_SoundManager.IsPlaying`, not component-local (`KidGameContent_functions.txt:76092-76103`). `KidPlayerSpecialMove_Jigoro.PreCutSceneState.OnTickSpecialMove` likewise changes state as soon as a requested girl voice is not reported playing (`KidGame_functions.txt:130749-130777`). | If native playback is queued and does not become visible to `IsPlaying` synchronously, the caller can miss the start and advance. More frequent polling narrows the interval between start and the next state decision. | Medium-low as a general explanation; high-value to instrument because the code has no “observed playing” latch. |
| 5 | Frame-counted Kismet actions upstream of audio | `MissionComplete.Update`, `MissionWave.Update`, `TicketGet.Update`, and `WeaponGet.Update` use render-frame counters rather than `DeltaTime`; at 240 FPS their nominal waits are 8 times shorter. | A level sequence can wire these latent outputs to a dialogue transition or stop. The audio code is innocent, but the map reaches the next action early. This is data-dependent and will affect only clips attached to those flows. | Medium for mission/reward dialogue, low elsewhere. |
| 6 | Channel pressure, concurrency, or one-sound-per-actor eviction | The active XAudio2 configuration has `MaxChannels=64`; native code implements single-sound-per-actor handling. The actual victim-selection policy is not visible in cooked script. | A frame-dependent effect or notify that emits many duplicate sounds can exhaust channels or repeatedly replace a same-actor sound. Voice then disappears without a script-level timer. | Low-medium. Static FPS alone should not alter the sample clock, so this requires an accompanying event-emission bug. |

One narrow anomaly is worth retaining in the test log: `KidGigoroGirl_Base.PlayGigoroGirlVoice` assigns `m_Reserve.Duration = 0.0` before playback (`KidGame_functions.txt:49487-49494`). This is the only voice-cue duration mutation found in cooked gameplay code. It is not itself frame-dependent and may simply force native duration refresh, so it is not a general explanation. If every affected clip is a Gigoro-girl line, however, the native manager's interpretation of that zero should be checked alongside component replacement.

## Animation-notify stop behavior

`KidAnimNotify_PlaySound.Notify` is an explicit voice/effect lifetime owner. After playing a cue it schedules `OnNotifyForceEnd` for the animation playback length divided by global play rate (`KidGame_functions.txt:67820-67888`). `ForceNotifyEnd` then stops the cue if the handle still matches and the cue is playing (`KidGame_functions.txt:67910-67920`). `KidAnimNotify_NormalEmCustomPlaySound` uses the same pattern.

This is a real cutoff boundary, but it is **not presently a direct frame-rate bug**:

- The sound notify passes `iWaitFrame = 0` to `OnNotifyForceEnd`.
- `KidPawn.TickNotifyTimer` uses `DeltaTime`, or animation `CurrentTime - PreviousTime`, once the wait-frame field is zero (`KidGame_functions.txt:22746-22805`).
- The known raw `miWaitFrame--` defect therefore does not affect the ordinary sound-notify call shown above.

An affected animation can still end early because of a different gameplay/state defect, and custom data could use a nonzero wait frame, but globally slowing every sound notify would be unjustified.

## Correctly time-based reference paths

Several callers demonstrate the intended design and help rule out a universal audio timer defect:

- `KidSeqAct_GigoroGirlCommunication.Update` latches `m_bPlay` only after it has observed the cue playing, and completes only after a later false result (`KidGameContent_functions.txt:82859-82889`). This avoids the start-observation race.
- `KidSeqAct_ScarletCutScene.MessageSceneState` adds an actual `DeltaTime` counter and a 0.5-second guard before it accepts a non-playing result (`KidGameContent_functions.txt:83468-83504`).
- Mondo's telephone path waits on the persistent component's `IsPlaying` state (`KidGame_functions.txt:127905-127920`) and uses ordinary engine timers for delayed starts.
- Boss voice calls reviewed in attack states use `SetRtnTime`/`IncRtn`; prior audit work established that `IncRtn` consumes the controller's current `DeltaTime`.

These paths can still be interrupted by an explicit replacement or state exit, but their nominal duration arithmetic is not render-frame-counted.

## Mechanical-filter results and false positives

The complete frame-step candidate list produced no direct cooked function of the form “voice time/count minus one per tick.” The audio-named matches were:

- `KidGFx_MovieSubtitle.Tick`: the raw `+8` operation is a debug display X coordinate, not time. It passes `DeltaTime` into native text-update functions (`KidGame_functions.txt:103690-103734`). Subtitle timing does not issue a sound stop.
- `KidPlayerSpecialMove_Jigoro.PlayLeverSoundSE` and `KidSeqAct_ScarletCutScene.PlayLeverSoundSE`: their unused declared delta does not make them voice-lifetime integrators, but their per-frame callers do make their feedback cadence frame-dependent; they are covered by `SoundRetriggerFix`.
- `KidPlayerSpecialMove_HamadayamaBike.Tick_Audio`: the suspicious `0.3` is a parameter/spatial adjustment; the sequence's 4.5-second timer is DeltaTime-driven.

The 30 Hz GFx bridge can affect subtitle presentation, but cooked subtitle code provides no route from text expiry back to audio playback. If only the text disappears early, that is a UI/subtitle problem; if sound becomes silent, it is one of the playback-ownership paths above.

## Configuration observations

The active device is `XAudio2.XAudio2Device` with `MaxChannels=64`, `CommonAudioPoolSize=320`, and `MinCompressedDurationGame=0.3` (`KidGame/Config/KidEngine.ini:364-377`). The last setting controls the threshold between decompression/streaming strategies; it is not evidence of a render-frame lifetime. Nothing in the reviewed INI values explains a deterministic “duration divided by FPS/30” cutoff.

Both `DevAudio` and `DevAudioVerbose` are suppressed in the active config (`KidGame/Config/KidEngine.ini:290-291`), so the current logs cannot distinguish explicit stops, concurrency eviction, streaming completion, or component reuse.

## Recommended proof step

Do not apply a global duration multiplier, fixed audio timestep, or unconditional `IsPlaying` override. Those would hide sequence bugs, extend intentionally interrupted lines, and risk breaking legitimate overlaps.

Add an opt-in diagnostic mode to the mod and test the same affected line at 30, 60, and 240+ FPS. Log monotonically timestamped events for:

- cue, component, owner, sound class, FaceFX state, and caller/return address;
- `Play`, `PlayAudioComponent`, component `Play`, `Stop`, `StopAudioComponent`, component `Stop`, and `StopActorFaceFXAnim`;
- `IsPlaying` false-to-true and true-to-false transitions;
- `OnAudioFinished`, Kismet `BeforeEnd`/`Finished`, and sound-marker callbacks;
- current game `DeltaTime`, world time, and the owning state/sequence object.

The first event at the audible cutoff identifies the layer:

| First event at cutoff | Root-cause class |
| --- | --- |
| Explicit stop or `StopActorFaceFXAnim` | Early caller/state/animation transition |
| New play on the same component/actor | Replacement, duplicate activation, or concurrency policy |
| Kismet `Finished`/marker before component completion | Native latent-sequence timing |
| `OnAudioFinished` with no prior stop/replacement | Backend, stream, channel eviction, or corrupt duration metadata |
| No audio event; subtitle alone vanishes | Subtitle/UI presentation |

Once one affected cue is identified, the fix should target the earliest incorrect event producer. The evidence does not support rate-limiting the entire audio manager.
