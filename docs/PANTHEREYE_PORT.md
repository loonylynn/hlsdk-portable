# Panther Eye integration (first pass)

This branch adds an initial, portable-SDK implementation of the Absolute Zero Panther Eye as a server-side monster entity.

## Entity

- Map classname: `monster_panthereye`
- C++ class: `CPantherEye`
- Model: `models/panthereye.mdl`
- Build source: `dlls/panthereye.cpp`
- Declaration: `dlls/panthereye.h`

The entity is registered through `LINK_ENTITY_TO_CLASS` and included in the server DLL CMake source list.

## Included in this first pass

- Alien predator classification.
- Panther Eyes treat other Panther Eyes as allies.
- Alien prey is treated as a lesser hostile target.
- Stock monster scheduler integration.
- Close claw melee attack using the model's event 1.
- Timed leap attack with a ballistic launch and touch damage.
- Original-style leap cooldown ranges by skill level.
- Increased hearing sensitivity and fast turning.
- Idle, attack, hit, miss, alert, pain and death sounds.
- Sequence mapping for the original Panther Eye model's idle, walk/run, claw, leap and violent-death animations.
- Optional distance-based cloak controlled by `panthereye_cloak`.

## Skill variables

The entity reads the usual Half-Life skill level (1-3):

| Cvar | Easy | Medium | Hard |
|---|---:|---:|---:|
| `sk_panthereye_health` | 80 | 100 | 120 |
| `sk_panthereye_dmg_claw` | 20 | 25 | 30 |

The optional server cvar `panthereye_cloak` defaults to `0` (disabled). Values above zero enable opacity-based cloaking when the creature has an enemy, is grounded, is not in its brief alerted/running window, and is at least 170 units from the enemy. A value of `1` leaves it fully opaque; smaller values make it more transparent.

## Required game assets

This code does not add proprietary game assets. Supply the model and sounds in the mod's game directory:

- `models/panthereye.mdl`
- `sound/panthereye/pa_attack1.wav`
- `sound/panthereye/pa_idle1.wav` through `pa_idle4.wav`
- `sound/panthereye/pa_death1.wav`

The implementation also references standard Half-Life zombie claw / alert / pain sounds.

The model is expected to contain sequences named `subtle_motion`, `itch`, `shakes`, `walk`, `run`, `attack_main_claw`, `attack_simple_claw`, `crouch_to_jump`, and `death_violent`. If a sequence is absent, the SDK's normal activity lookup is used as fallback; the animation may therefore differ from Absolute Zero.

## Scope and known omissions

This is an initial gameplay port, not a behavior-identical transplant of Absolute Zero's 3,400-line implementation. It deliberately avoids Absolute Zero's custom base-monster framework, hidden-CVar system, custom animation event queue, debug-drawing utilities, and modified movement/pathfinding APIs.

Not yet ported:
- Full stalking / sneaking and cover-seeking state machine.
- Original fight-or-flight logic based on whether the enemy is looking at the Panther Eye.
- Original bug-eating behavior.
- Full custom flinch/death handling and movement-bound fixes.
- Exact animation weighting / randomized sequence timing.
- Map-editor FGD definitions and any custom asset package.

The code has been adapted to the stock `CBaseMonster` API in hlsdk-portable, but it has not been compiled or tested in-engine in this integration environment. Treat it as a first integration pass to compile and tune locally before using it in maps.
