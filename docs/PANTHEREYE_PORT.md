# Panther Eye integration

This branch contains a partial, portable-SDK implementation of the Absolute Zero Panther Eye as a server-side monster entity. The core stalking, stealth, ambush and combat loop is integrated, while the remaining differences and unported behavior are listed below.

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
- Stalking state that pauses in an idle/fidget when the enemy is looking directly at it.
- Stalking movement that prefers lateral occlusion and navigation-node cover, then falls back to a point behind the target.
- A prolonged-stare response that breaks stealth and initiates a short aggressive pursuit.
- An ambush leap when the target looks away at practical leap range.
- Fight-or-flight damage response: close-range damage provokes an attack run; distant damage triggers a cover-seeking escape.
- A cornered state that makes the Panther Eye easier to provoke.

## Skill variables

The entity reads the usual Half-Life skill level (1-3):

| Cvar | Easy | Medium | Hard |
|---|---:|---:|---:|
| `sk_panthereye_health` | 80 | 100 | 120 |
| `sk_panthereye_dmg_claw` | 20 | 25 | 30 |

The optional server cvar `panthereye_cloak` defaults to `0.35` (a subtle partial cloak). Set it to `0` to disable cloaking; values above zero enable opacity-based cloaking when the creature has an enemy, is grounded, is not in its brief alerted/running window, and is at least 170 units from the enemy. A value of `1` leaves it fully opaque; smaller values make it more transparent.

## Timed bleeding

Panther Eye claw hits apply Absolute Zero's timed bleeding effect:

- Both claw strikes go through `DoClawHit()`. The first strike uses full claw damage; the follow-up uses half claw damage. Both pass `DMG_SLASH` as the engine damage type and `DMG_BLEEDING` as the separate secondary damage modifier.
- The airborne leap attack is unchanged and does **not** inflict bleeding, matching the original Absolute Zero behavior.
- Bleeding is tracked with a dedicated secondary mask, remaining-tick counter and next-tick time. It uses a two-second interval and does not occupy or renumber GoldSrc's original eight time-based damage slots.
- A new wound starts its first tick two seconds later. Further claw hits refresh the remaining duration without postponing an already scheduled tick.
- The timed damage and blood-drip effect are processed for players from `CBasePlayer::PreThink()`. Monster bleeding is processed from `CBaseMonster::MonsterThink()` when `timedDamageAffectsMonsters` is nonzero.
- Bleeding state and its remaining time are included in monster save/restore data and are reset on player respawn / monster initialization.

### Bleeding skill cvars

These skill cvars are registered for easy, medium and hard difficulty:

| Cvar | Default on each skill level | Meaning |
|---|---:|---|
| `tdmg_bleeding_duration` | `1` | Number of two-second bleeding ticks |
| `tdmg_bleeding_damage` | `1` | Damage applied on each tick |

With the defaults, a wound causes one tick of 1 damage after approximately two seconds. A duration of zero or a negative value disables bleeding in this port. Adjust the numbered cvars in the mod's skill configuration (for example, `tdmg_bleeding_duration1` and `tdmg_bleeding_damage1`) to tune each difficulty independently.

The server cvar `timedDamageAffectsMonsters` defaults to `1`. Set it to `0` to disable timed bleeding on monsters; player bleeding is unaffected.

### Player feedback and cures

- When a player first starts bleeding, the HEV suit queues `!HEV_DMG6` ("blood loss detected"). Refreshing an existing wound does not queue the warning again.
- A world first-aid kit cures bleeding after it successfully restores health.
- Activating a wall-mounted `func_healthcharger` attempts to cure bleeding even if that activation cannot restore health, matching Absolute Zero's behavior.
- A successful cure clears the bleeding mask, remaining ticks and scheduled tick time. The HEV suit randomly reports either `!HEV_HEAL6` (wound sterilized) or `!HEV_HEAL7` (morphine shot); both alternatives share a 30-second repeat block.

The bleeding implementation is integrated. The full SDK server and client targets have now both compiled successfully on Debian 13, producing 32-bit i386 ELF shared objects (`build/dlls/hl.so` and `build/cl_dll/client.so`). This confirms source compilation and linking, but the Panther Eye and bleeding behavior have **not yet been tested in-game**. In particular, verify claw hit registration, tick timing and damage, cure behavior, HEV sentence playback, save/load continuity, and the monster cvar in a local GoldSrc/Xash3D build.

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

The following original Panther Eye features remain unported or are only approximated:
- The entire original stalking state machine, including its exact timers, thresholds, dynamic path throttling and bespoke schedule transitions. The current implementation reproduces the core watch/pause, sneak/cover, ambush and flee loop using stock schedules.
- Original bug-eating behavior.
- Full custom flinch/death handling and movement-bound fixes.
- Exact animation weighting / randomized sequence timing.
- Map-editor FGD definitions and any custom asset package.

The code has been adapted to the stock `CBaseMonster` API in hlsdk-portable. The server and client targets compile successfully as 32-bit Linux shared libraries, but Panther Eye behavior has not yet been tested in-engine. The stalk/flee tasks depend on the SDK's normal navigation and local-cover checks; maps without a usable node graph can use the lateral-cover and behind-target fallbacks, but path success still needs in-engine testing. Test locally before using it in maps.
