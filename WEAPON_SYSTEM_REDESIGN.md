# Weapon System Redesign

## Status

Proposed architecture for the single-player Xash3D project. This document is a design target, not a claim that the replacement runtime has already been implemented.

## Scope and priorities

The immediate goal is to replace the difficult parts of the GoldSrc weapon architecture for the built-in Half-Life weapon set and this project's own weapons.

In scope:

- A consistent way to define, tune, equip, fire, reload, holster, save and restore weapons.
- Reusable mechanics so adding a conventional weapon does not require copying a whole weapon class.
- Native C++ behaviour for mechanics that need custom logic.
- A small, validated JSON definition for built-in weapon data and assembly.
- Compatibility with the current Xash3D player, HUD, ammo and entity interfaces during migration.

Explicitly deferred:

- Third-party weapon packages and a public modding API.
- Luau or another embedded scripting language.
- Multiplayer support as a design target.
- Rewriting every unique weapon mechanic as data.

## What the current SDK makes difficult

The current implementation spreads one weapon across several systems:

- Each weapon has a C++ entity class with its own `Spawn`, `Precache`, `GetItemInfo`, `Deploy`, attack, reload, idle and holster functions.
- `CBasePlayerWeapon::ItemPostFrame()` polls input and dispatches virtual callbacks, while individual weapons override frame handling for special reloads, pumps, charge states and other exceptions.
- The player stores linked lists of weapon entities by slot, an active item pointer, and ammo in fixed-size arrays.
- Legacy numeric weapon IDs are coupled to HUD masks and `ItemInfoArray`.
- `W_Precache()` manually enumerates weapon and ammo entity classnames.
- Ammo summary fields are maintained through explicit hardcoded calls in `CBasePlayer::TabulateAmmo()`.
- `CLIENT_WEAPONS=ON` builds the weapon code for both client and server DLLs. The replacement must preserve a coherent prediction/presentation strategy rather than assume single-player removes the client/server split.

The current JSON work moves metadata out of several classes, but does not yet replace this lifecycle architecture.

## Architectural direction

Use five distinct layers.

### 1. Weapon definitions

Immutable, validated data loaded once at game startup. A definition describes:

- Stable internal identifier and legacy compatibility ID.
- Inventory slot, order, selection and weight.
- Ammo resource, clip capacity, reserve capacity and starting amount.
- World, view and player models.
- Animation and sound presentation.
- Primary and secondary action configuration.
- Reload policy.
- Reusable behaviour components and their parameters.

JSON is the authoring/configuration format for the built-in weapons. It is not an arbitrary scripting language. Runtime code must not repeatedly parse JSON while firing or switching weapons.

Skill-dependent damage should continue to use the project's skill system where appropriate. A weapon definition can refer to a skill damage key/profile rather than silently replacing difficulty scaling with a fixed number.

### 2. Weapon instances

A weapon instance represents one owned weapon and contains only runtime state, for example:

- Definition ID.
- Clip / loaded resource count and reserve references.
- Current lifecycle state.
- Primary and secondary cooldowns.
- Reload progress.
- Firing or charging state.
- Per-instance behaviour state.

Definition data must never be mutated to store player-specific state. Runtime state must be save-game serializable and versionable; raw pointers and transient engine handles are not a save format.

### 3. Weapon controller

A per-player controller owns the active-weapon lifecycle and centralises:

- Give, remove, equip, holster and switch.
- Input routing for primary, secondary, reload and weapon selection.
- Cooldowns and action scheduling.
- Empty-ammo rules and auto-reload/auto-switch policy.
- The transition between deploy, ready, firing, reloading, charging and holstered states.
- Notifications to HUD, viewmodel and audio presentation.

A weapon should not need to reimplement the common lifecycle just to change its firing mechanic.

### 4. Reusable behaviour modules

Keep mechanics in C++, behind small reusable interfaces. A weapon definition composes the modules it needs.

Initial behaviour families to design for:

- Hitscan: single shot, burst, automatic and pellet spread.
- Projectile: launch, projectile configuration and impact handling.
- Melee: range, hull/trace shape, hit timing and impact response.
- Continuous beam: start, sustain, stop and resource drain.
- Charge: begin, accumulate, release, cancel and overcharge.
- Deployable / throwable: place or throw an entity and manage ownership.
- Remote detonation / alternate mode.
- Reload policies: magazine, per-shell, resource-based, no-reload and custom staged reload.

These are behaviour families, not a requirement to build every module up front. Special cases should be expressed as small dedicated components, not by adding weapon-name checks to the central controller.

### 5. GoldSrc/Xash compatibility adapter

During migration, keep the engine-facing responsibilities behind an adapter:

- Legacy weapon IDs and bitmasks for existing HUD messages.
- Ammo inventory integration.
- Existing pickup/map entity classnames.
- Viewmodel and animation messages.
- Save/restore integration.
- Client weapon prediction and server gameplay effects.

This adapter is temporary compatibility infrastructure, not the design that new weapon code should depend on.

## Prediction and single-player

This project is single-player, but the current build still has client and server weapon code, with client weapon prediction enabled. The first replacement should keep shared, deterministic state transitions and clearly separate predicted presentation from authoritative world effects.

Do not turn prediction off just to make the rewrite easier. First identify what the current engine and client DLL require, then make any change to prediction deliberately and test it for input latency, firing cadence, animations, save/load and weapon switching.

## Migration strategy

Migrate incrementally. Keep the old implementation available until each replacement passes the same gameplay tests.

1. **Contracts and observability.** Finalise definition/runtime/controller interfaces. Add an on-demand diagnostic command that lists definitions and their effective values.
2. **Runtime foundation.** Add instance state, lifecycle state machine, input routing and save/restore support without changing the visible weapon behaviour.
3. **First built-in pilot.** Port one relatively conventional weapon through the new controller. Compare ammo use, timing, animations, switching, prediction and save/load against the old implementation.
4. **Reusable action modules.** Add hitscan, projectile and melee modules, then the reload policies required by stock weapons.
5. **Complex weapons.** Migrate the weapons with distinctive state: shotgun per-shell reload, MP5 alternate grenade, crossbow zoom/reload, RPG guidance, Gauss charging, Egon continuous fire, satchel radio state, tripmine placement and snark deployment.
6. **Remove legacy boilerplate.** Only after all built-in weapons pass validation should their individual weapon classes and duplicate lifecycle code be removed.
7. **Simplify legacy metadata.** Retire fixed tables, hardcoded precache lists and compatibility glue only when their engine/HUD responsibilities have a replacement.

## Acceptance criteria

A replacement is not complete just because it compiles or the weapon appears in the player's inventory. Each migrated weapon must be checked for:

- Correct map pickup and starting ammo.
- Correct selection order, switching, deploy and holster.
- Correct view/world/player models and animations.
- Primary and secondary attacks, including empty and underwater cases.
- Reload timing, interruption and reserve accounting.
- Weapon-specific states and spawned entity ownership.
- Save/load restoration mid-action.
- Client/server agreement and stable prediction.
- No regressions to existing HUD, ammo pickups or map entities.

## Design rules

- Prefer composition of reusable mechanics over one class per weapon.
- Prefer explicit data over scattered constants and duplicated boilerplate.
- Keep unique behaviour in focused C++ components; do not force it into JSON.
- Keep gameplay state separate from static definitions.
- Do not add an addon/mod API requirement to solve an internal architecture problem.
- Preserve the original game feel unless a deliberate gameplay change is requested.
- Make one weapon migration at a time, with build and in-game checks before removing old code.
