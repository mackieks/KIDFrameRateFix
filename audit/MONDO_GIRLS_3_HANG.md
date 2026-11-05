# Mondo Girls 3 high-frame-rate hang

## Conclusion

The freeze is an infinite linked-list traversal in the native UE3 ribbon-particle code. It is caused by an interaction between `ParticleModuleCollisionActor` and `FParticleRibbonEmitterInstance`, not by the movie, animation, Kismet, Scaleform, or the existing frame-rate-fix plugin.

The collision module is authored to kill a particle after 50 collision contacts. Its native update removes that particle from the emitter's active-index array with a generic swap/decrement operation. It does not unlink the particle from the ribbon's packed previous/next chain. When the dead direct index is later reused for a new ribbon head, an older particle's stale link points into the new generation and closes the chain into a ring. `Tick_RecalculateTangents` expects a terminating `0x3FFF` link, so it traverses the ring forever.

The apparent 30 FPS dependency comes from the collision budget, not from a bad DeltaTime multiplier in tangent recalculation. The budget is decremented by one per qualifying collision update. The affected particles have a 1.5-second lifetime and a 50-contact budget. At 30 FPS they receive at most 45 updates over their entire life, so collision completion cannot occur and the ribbon-aware lifetime killer removes them safely. At sufficiently high frame rates, 50 contacts can be consumed before the lifetime expires, exposing the unsafe collision-removal path.

## Affected asset

The live emitter resolves to:

`FX_FMV_Liquid.Effects.FX_FMV_Liquid_ShishiodoshiWaterFall.ParticleSpriteEmitter_0`

This is the shishi-odoshi waterfall ribbon in the Koharu garden. Its active LOD contains:

- `ParticleModuleEventGenerator`
- `ParticleModuleLifetime`
- `ParticleModuleSize`
- `ParticleModuleVelocity`
- `ParticleModuleVelocityOverLifetime`
- `ParticleModuleAcceleration`
- `ParticleModuleColorOverLife`
- `ParticleModuleCollisionActor`
- `ParticleModuleTypeDataRibbon`

Cooked values recovered from `ENV_S02_Gigolo_Koharu`:

| Property | Value |
| --- | ---: |
| Lifetime | 1.5 seconds |
| Spawn rate | 10 particles/second |
| Collision actor parameter | `Bamboo` |
| MaxCollisions | 50 (uniform minimum and maximum) |
| CollisionCompletionOption | `EPCC_Kill` (enum value 0/default) |
| Ribbon MaxParticleInTrailCount | 25 |
| bTangentRecalculationEveryFrame | true |

## Frozen-dump evidence

Dump: `hangs/KidGame-60fps-frozen-procdump.dmp`

The module was loaded at `0x00950000`, a relocation of `+0x00550000` from the PE's preferred image base.

The hot thread was at actual address `0x01B32875` (preferred `0x015E2875`) inside the helper called by `FParticleRibbonEmitterInstance::Tick_RecalculateTangents`. That helper repeatedly follows the low 14-bit next index until the `0x3FFF` sentinel. It contains no cycle or iteration bound.

Live emitter state:

| Field | Value |
| --- | ---: |
| Emitter instance | `0x47C93700` |
| ParticleData | `0x1BAF0000` |
| ParticleIndices | `0x57510780` |
| Particle stride | `0xF0` |
| Ribbon payload offset | `0x80` |
| Collision payload offset | `0xD0` |
| ActiveParticles | 15 |

The exact next-index traversal was:

`2 -> 1 -> 0 -> 14 -> 10 -> 9 -> 8 -> 13 -> 12 -> 11 -> 7 -> 6 -> 5 -> 4 -> 3 -> 2`

Index 3 was the oldest live particle (normalized relative time about 0.95), but its next field was the ordinary index 2 rather than `0x3FFF`. Index 2 had already been recycled as the newest head (relative time about 0.0167). This is a cross-generation stale link, not a legitimate trail topology.

The collision payload's integer at `+0x18` is initialized to 50 and is visibly depleted in the live particles:

| Relative time | Actual age | Contacts remaining |
| ---: | ---: | ---: |
| 0.4833 | 0.725 s | 49 |
| 0.5500 | 0.825 s | 45 |
| 0.6167 | 0.925 s | 34 |
| 0.6833 | 1.025 s | 36 |
| 0.7500 | 1.125 s | 29 |
| 0.8167 | 1.225 s | 22 |
| 0.8833 | 1.325 s | 18 |
| 0.9500 | 1.425 s | 13 |

The variation is expected from the moving collision actor and particle trajectories, but the overall relationship is clear: after contact begins, the integer is consumed approximately once per render update. The earliest observed trajectory is consistent with reaching zero near 60 FPS. At several hundred FPS the same budget can disappear much earlier in wall-clock time, explaining the much faster freeze.

## Native control flow

Preferred addresses in `KidGame.exe`:

1. `FParticleRibbonEmitterInstance::Tick` at `0x01608620` first calls its ribbon-aware virtual `KillParticles` at vtable slot `+0x88` (`0x015FF8A0`).
2. That routine removes particles whose normalized relative time is strictly greater than 1.0 and repairs the ribbon neighbor links before swapping the direct index out of the active array.
3. Tick then spawns particles and runs update modules, including `ParticleModuleCollisionActor`.
4. The generic collision update is at `0x015DADE0`.
5. On a qualifying collision, `0x015DC0C3` or `0x015DC0E4` decrements the per-particle integer at collision payload `+0x18`.
6. `0x015DC0ED` tests that integer, and the `<= 0` path reaches collision completion.
7. `0x015DCFC2` loads `CollisionCompletionOption` from module offset `+0x9C`. Enum value 0 selects `EPCC_Kill`.
8. `0x015DD005..0x015DD035` removes the direct particle index by swapping with the last active entry and decrementing `ActiveParticles`. There is no access to the ribbon payload and no neighbor repair.
9. The same emitter tick later calls `Tick_RecalculateTangents`, which encounters the now-cyclic chain.

This ordering also explains why the ordinary lifetime cleanup cannot repair the damage later: collision removal occurs after the ribbon-aware kill phase, and tangent recalculation occurs before the next tick.

## Correct repair direction

The structural fix belongs at the collision-completion kill path. When the emitter is a ribbon emitter, the selected particle must be unlinked using the same START/ONLY/END/MIDDLE neighbor rules as `FParticleRibbonEmitterInstance::KillParticles` before the generic active-index swap occurs. Non-ribbon emitters should retain the original path.

That repair removes the invalid state at its source and remains correct at any frame rate. A traversal cap in `Tick_RecalculateTangents` would only hide the corruption and could leave malformed geometry elsewhere.

There is a separate fidelity issue: a continuously contacting particle consumes one `MaxCollisions` unit per rendered update, so its visual lifetime after contact is frame-rate dependent even after the structural crash is fixed. If exact 30 FPS behavior is desired, continuous-contact budget consumption can be normalized with a per-particle `DeltaTime * 30` accumulator. That should be treated as a second change, not substituted for the required ribbon unlink repair.

## Scope

The initial investigation changed no active mod binary, plugin source, or game configuration. The diagnostic dump reader and package-object dumper were used to inspect the frozen process and cooked asset data.

## Implemented repair

`ParticleCollisionFix=1` now hooks the uniquely validated `UParticleModuleCollision::Update` entry point. For `EPCC_Kill` ribbon emitters it temporarily substitutes a non-expiring collision counter, allowing the native collision response to report whether each particle actually contacted something without entering the unsafe generic removal branch. The plugin then:

1. advances a private collision-depletion accumulator by `DeltaTime * 30` for each qualifying contact;
2. preserves the first discrete contact as an immediate one-count decrement;
3. restores the resulting logical integer count to the native payload; and
4. when the logical count reaches zero, marks the selected particle expired and calls the emitter's validated ribbon-aware virtual `KillParticles` routine.

At 30 FPS, consecutive contacts still decrement the native logical budget once per update. At 60, 120, 240 FPS and beyond, the same 50-contact budget spans approximately the same 1.633-second interval from the first through final sample. Non-ribbon emitters and non-`EPCC_Kill` completion modes retain retail behavior.
