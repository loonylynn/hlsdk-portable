# Weapon System Redesign

## Status

Proposed architecture for the single-player Xash3D project. This document is a design target, not a claim that the replacement runtime has already been implemented.

## Scope and priorities

This is not a Half-Life remake or a project that intends to preserve the Half-Life weapon roster. The existing HL1 weapons are a temporary compatibility baseline and a useful set of test cases. The actual goal is a clean, reusable weapon foundation for this project's own original weapon roster, allowing most stock weapons to be replaced over time.

In scope:

- A consistent way to define, tune, equip, fire, reload, holster, save and restore weapons.
- Reusable mechanics so adding a conventional weapon does not require copying a whole weapon class.
- Native C++ behaviour for mechanics that need custom logic.
- A Source 1-style, manifest-loaded database of validated JSON weapon definitions, parsed once into typed runtime data.
- Reusable native C++ behaviour implementations selected by each definition.
- Compatibility with the current Xash3D player, HUD, ammo and entity interfaces during migration, without making those legacy interfaces the permanent design.

Explicitly deferred:

- Third-party weapon packages and a public modding API.
- Luau or another embedded scripting language.
- Multiplayer support as a design target.
- Rewriting every unique weapon mechanic as data.

## Selected foundation: Source 1's weapon-info and lifecycle pattern

Use the practical Source 1 pattern as the foundation: a manifest identifies per-weapon scripts, the scripts are parsed once into typed weapon-info objects, and a shared native weapon lifecycle dispatches to focused C++ implementations. Keep the file format as strict JSON rather than copying Valve's KeyValues parser. The important part is the contract and separation, not the syntax.

This means:

- One canonical weapon definition per weapon, loaded and cached at startup.
- A typed common weapon-info structure for identity, inventory, ammo, models, presentation and attack/reload configuration.
- A native behaviour key in each definition, resolved through an explicit C++ behaviour registry.
- One shared lifecycle for input, deploy/holster, busy states, attack scheduling, reload transitions and idle handling.
- Focused native implementations for genuinely distinct mechanics. A different damage value, clip size, model or fire rate must not require a new weapon class.

Source 1 is the selected architecture. Source 2 VData and Alyx's generic pistol remain useful comparisons, but they are not implementation targets for this project.

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



## Implementation checkpoint

The shared contracts and the first native hitscan dispatch slice now exist on `weapon-framework/foundation`:

- `weapon_runtime.h` defines input snapshots and pointer-free per-instance lifecycle/reload/ammo/timer state.
- `weapon_behavior.h/.cpp` defines the native behaviour callback contract, engine-action host, keyed custom-state archive, and stable-key behaviour registry.
- `WeaponDefinition` exposes an optional native behaviour key. A JSON `hitscan` action resolves to `firearm.hitscan`.
- The built-in `firearm.hitscan` behaviour is registered lazily by the shared registry. It supports semi-automatic press-edge input and automatic held input, attack cooldowns, clip consumption after a successful engine action, and fire presentation callbacks.
- The existing generic JSON hitscan adapter dispatches through the native behaviour and translates the request to GoldSrc's bullet, animation, sound and muzzle-flash APIs. Its previous firing implementation remains as a fallback if no behaviour resolves.
- Both client and server DLL targets include the behaviour registry and the generic adapter. Focused tests cover registration/validation, semi-auto input edges, cooldowns, ammo consumption, rejected shots and invalid contexts.
- `CFrameworkWeapon` now owns a persistent `WeaponInstanceState` instead of creating a temporary state for every shot. Its state is synchronized around the existing GoldSrc `ItemPostFrame` loop, with the legacy clip and attack timers retained as the compatibility/prediction transport.
- The generic entity has a server-only class-specific save description for state version, lifecycle, reload phase, loaded ammo, attack/idle timers, action timestamps and action sequence. The client prediction copy keeps no-op Save/Restore virtual stubs and does not link the server-only save buffer implementation.
- Restore validates the state version and ranges. For an older save without the `CFrameworkWeapon` field block, the SDK reader rewinds when the class marker is absent and the adapter reconstructs runtime state from the legacy clip/timer/reload fields.
- The current semi-automatic action end time is reconciled from the existing primary attack timer, which is already transported through `weapon_data_t`. Future behaviour-specific custom state will need explicit client prediction mapping.
- Focused tests now cover runtime-state validity, reset, value-copy preservation and rejection of invalid versions/enums/ammo/timer values, in addition to the native firing behaviour tests.

**Current boundary:** this is persistent runtime state for the generic hitscan entity, not the completed player weapon controller. It still uses GoldSrc inventory ownership, clip/ammo arrays, input processing and attack-timer transport. The custom behaviour-state archive is not yet connected to the engine save system. Reload is tracked by the adapter while GoldSrc continues to perform the actual magazine reload. The stock Glock remains on its original C++ implementation; its custom trigger-release cadence, accuracy recovery and event-based firing have not been migrated. The new changes still require a fresh focused test run and 32-bit client/server build before they are considered validated.

## Valve references and lessons (research, 2026-11-26)

This section records patterns observed in Valve's public Source SDK code, Source 2 weapon data, and Half-Life: Alyx Workshop materials. These are references for design, not a requirement to reproduce Valve's engine architecture.

### Source SDK 2013 (Source 1)

Valve's Source SDK uses a cached FileWeaponInfo_t database loaded from a weapon manifest. Each weapon script is parsed once into a typed data object. The base data includes class/display names, view and world models, animation prefix, inventory bucket and position, clip/default-clip sizes, weight, auto-switch rules, flags, ammo types, sound categories, and HUD icons. The parser is virtual so a game can derive its own weapon-info type and parse additional game-specific values.

References:
- [Valve weapon_parse.cpp](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/shared/weapon_parse.cpp)
- [Valve weapon_parse.h](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/shared/weapon_parse.h)

The Source CBaseCombatWeapon base class also provides more lifecycle extension points than GoldSrc's minimal item loop: pre/post-frame, busy-frame, holster-frame, deploy/holster, empty-fire handling, reload checks/finish/abort, primary/secondary attacks, and attack-activity selection. Individual weapons still implement specialised behaviour in C++.

References:
- [Valve basecombatweapon_shared.h](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/shared/basecombatweapon_shared.h)
- [Valve HL2 shotgun implementation](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/server/hl2/weapon_shotgun.cpp)
- [Valve HL2 RPG implementation](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/game/server/hl2/weapon_rpg.h)

**Lesson:** build a real shared lifecycle with well-defined extension hooks, not a single giant switch statement. Keep the weapon definition database separate from the runtime weapon instance and from the code that executes attacks. Preserve escape hatches for unusual mechanics.

### Source 2 and Counter-Strike 2

Source 2 uses schema-described C++ data classes and typed VData assets. A .vdata asset supplies values for a known schema class; it is not an unrestricted free-form script. The schema includes typed fields and authoring metadata, such as editor labels, descriptions, valid ranges, and resource pickers.

CS2's CBasePlayerWeaponVData holds shared properties such as world model, muzzle attachment and effects, handedness/flipping, linked cooldown policy, item flags, auto-switch rules, ammo types, clip/default-clip sizes, reserve-ammo treatment, HUD slot/position, and sound events. CCSWeaponBaseVData extends it with CS-specific properties such as weapon category, bullet count, firing cadence, burst configuration, spread/inaccuracy curves, recoil values, zoom, damage, penetration, range, reserve ammo limits, and reload/deploy timing.

References:
- [CS2 CBasePlayerWeaponVData schema](https://s2v.app/SchemaExplorer/cs2/client/CBasePlayerWeaponVData)
- [CS2 CCSWeaponBaseVData schema](https://s2v.app/SchemaExplorer/cs2/client/CCSWeaponBaseVData)
- [CS2 weapon data file research and field mapping](https://cs2.gs/about/)
- [Source 2 schema and VData overview](https://www.source2.wiki/Basics/schemas)

**Lesson:** use a shared base definition plus specialised typed extension data. Put real weapon tuning into data: not only clip size and damage, but recoil, inaccuracy, firing modes, cooldown relationships, reload policy, animation/presentation hooks and sound/effect references. Validate values against a schema and provide editor-friendly descriptions/ranges. In our project, JSON can remain the authoring format initially; the important idea is the typed contract and separation of common versus game-specific properties.

### Half-Life: Alyx — the closest Half-Life reference

Valve added hl_vr_weapon_generic_pistol as a data-driven weapon entity for Workshop customization. Contemporary reporting on Valve's update describes it as reusing the unupgraded Alyx pistol's behaviour while exposing properties such as models, effects, sounds, damage and firing rates for override. Valve's update also included a Custom Pistols tutorial and example AnimGraph sources.

References:
- [Valve Half-Life: Alyx Update 1.4.1 notes](https://store.steampowered.com/news/posts/?appgroupname=Half-Life%3A+Alyx&appids=546560&enddate=1592349047&feed=steam_community_announcements)
- [Report on the generic pistol and its exposed properties](https://roadtovr.com/half-life-alyx-update-1-4-1-example-pistol-modding/)

**Lesson:** a small library of native weapon archetypes can carry reusable mechanics while a data definition changes their tunable properties and presentation. This is a better model for our near-term scope than either a unique C++ class for every weapon or trying to express all gameplay as JSON. For example, an ordinary semi-automatic pistol should use the generic pistol/magazine archetype; only a pistol with genuinely different mechanics should require a specialised behaviour component.

### What not to copy

- Do not recreate Source 2's full reflection/schema runtime. We need a small, explicit schema for our own weapon data, not an engine-wide type system.
- Do not make every weapon fully data-only. Valve's Source 1 and Source 2 patterns retain native classes for actual execution and specialised rules.
- Do not mistake CS2 map scripting (cs_script) for its weapon implementation. Its weapon definitions are VData, while weapon gameplay remains implemented by game-side weapon classes.
- Do not import CS2 competitive mechanics (recoil curves, armour penetration, economy, etc.) unless the Half-Life design actually needs them.
- Do not make a public modding API or scripting VM a prerequisite for cleaning up our internal weapon architecture.

### Resulting design decisions

1. Keep immutable, typed weapon definitions distinct from runtime state.
2. Support inheritance/composition of definition profiles (common base, archetype profile, per-weapon overrides) with deterministic defaults and validation.
3. Introduce a small set of native archetypes/behaviour modules for the common cases: pistol, automatic firearm, shotgun/per-shell reload, projectile launcher, melee, continuous beam, charge weapon, and deployable/remote device.
4. Keep all unique mechanics in focused native components attached to an archetype or controller state machine; avoid a per-weapon class just for data changes.
5. Make the controller own common lifecycle, input routing, switching and cooldown rules, while behaviour modules handle the mechanics of a particular action.
6. Store animation and presentation as authored data references, but let a behaviour explicitly request the appropriate action/activity so data does not become a giant brittle animation-number table.
7. Add debug inspection for the effective resolved definition and current instance state.
8. Migrate the built-in weapons one at a time and retain the original C++ implementation as a fallback until gameplay parity is verified.


## Source 1 implementation contract

This section turns the architectural choice into concrete boundaries. Names below are proposed API names; they are not claims that these classes already exist.

### Definition files and loading

Keep one definition file per weapon under `scripts/weapons/` and one explicit root manifest that lists the definitions to load. The loader parses each file once during initialization, validates it, resolves its native behaviour, and stores an immutable typed result. The runtime never parses JSON during firing, reloading, switching or save/load.

The current `packages.json` catalogue can remain as a temporary loader entry point while we migrate. Third-party packages, downloadable packages and a public package API are not part of this redesign. If the package indirection adds no value to the built-in roster, we can simplify it to a single manifest later.

A definition's canonical identity is its stable string name, for example `project:weapon/sidearm`. The following are compatibility bindings, not identity:

- `legacy_id`: optional numeric ID used by the existing GoldSrc HUD, inventory bitmask and weapon messages.
- `entity_classname`: optional old map/pickup classname used to spawn an existing stock entity.
- Ammo registry indices: assigned by the engine adapter from stable ammo names.

New definitions must not be required to pretend to be `weapon_glock`, `weapon_mp5` or any other stock classname. A generic map-spawn entity can carry a definition name for new weapon pickups while old map classnames continue to resolve through compatibility bindings.

### Typed data contract

The common typed definition should be shaped around these groups:

| Group | Responsibility |
| --- | --- |
| Identity | Stable name, display token, optional legacy ID and old entity aliases |
| Behaviour | Native behaviour/archetype key and explicitly supported options |
| Inventory | HUD slot/order, selection weight and switch policy |
| Ammo | Named ammo resources, clip capacity, reserve limits and starting amounts |
| Models | View, world and player models |
| Presentation | Animation activities/sequences, sound events, muzzle/effect references and animation prefix |
| Primary / secondary | Action mode and typed tuning values supported by that behaviour |
| Reload | Reload policy and its typed timing/animation parameters |

Fields that only make sense for one behaviour belong in that behaviour's typed data, rather than being added as unrelated fields to every weapon. For example, pellet count and spread belong to a hitscan-firearm configuration; charge duration belongs to a charge behaviour; staged shell insertion belongs to a per-shell reload policy.

The definition is immutable after loading. Missing optional values receive documented defaults. Unknown keys, duplicate keys, unsupported behaviour names, invalid ranges and incompatible field combinations fail validation with a useful diagnostic. Do not silently ignore a misspelled tuning property.

### Native behaviour contract

A definition selects a registered native behaviour by a stable key such as `firearm.semiauto` or `firearm.automatic`. The registry maps that key to a known C++ implementation. The JSON file cannot name arbitrary C++ symbols or instantiate arbitrary code.

The shared lifecycle owns the common rules and invokes behaviour callbacks at clear points:

- spawn/precache and initialisation;
- can-deploy, deploy and holster;
- pre-frame, post-frame and busy-frame processing;
- primary and secondary action requests;
- reload start, update, finish and abort;
- idle and empty-action handling.

A behaviour owns the mechanics of its action: hitscan traces, projectile launches, melee hit windows, beam sustain, charge/release or a special device. It does not independently reimplement weapon selection, inventory ownership, generic cooldown policy or the whole input loop. Do not replace the current per-weapon class problem with one giant `switch (weaponName)` statement; use an explicit behaviour registry and narrow interfaces.

### Runtime instance and save data

Each owned weapon has a runtime instance separate from the shared definition. Common runtime data includes loaded ammo, lifecycle/reload state, attack timers and any common action progress. Behaviour-specific state is stored in a versioned, serializable state block owned by that behaviour.

The behaviour implementation itself should be stateless where practical. If a behaviour object is reconstructed, it is selected again from the definition's behaviour key. Never save raw pointers, vtable addresses, registry addresses or transient engine handles. Save/load must preserve important mid-action cases such as a reload, charge, beam sustain or remote-device state.

### Lifecycle and controller boundary

The long-term owner of switching, action scheduling and input routing is one shared per-player weapon controller. The first implementation should integrate with the existing `CBasePlayer` and `CBasePlayerWeapon` frame loop rather than replacing the entire player inventory system in one step. We can move the implementation behind the controller boundary first, then replace linked-list inventory and fixed arrays only when their engine/HUD dependencies are mapped.

The lifecycle should expose distinct ready, deploying, holstered, busy, primary-action, secondary-action and reloading states as needed. Each state must have explicit entry/exit and interruption rules. A reload is not just a timer if it inserts rounds in stages; the common lifecycle provides the hooks, while the selected reload policy determines the sequence.

### Compatibility limits to isolate

The current GoldSrc layer uses `MAX_WEAPONS`, bit-based ownership, `ItemInfoArray`, fixed ammo arrays and numeric HUD messages. The new system uses stable string identities internally and maps to those old values only at the adapter boundary. This avoids spreading legacy IDs into every new class, but it does not magically remove the engine's fixed limits. Expanding beyond those limits requires a separate migration of player inventory, HUD messages and client data.

The current JSON parser also contains GoldSrc-specific item-flag and bullet-type values. Move those mappings to the compatibility/behaviour layer. Shared definition parsing should understand stable symbolic values and typed weapon concepts, not depend on GoldSrc enum numbers.

### First proof of the contract

Use a conventional semi-automatic pistol as the first vertical slice. The existing Glock is a convenient parity test because its current ammo, clip, animation and firing behaviour are known; using it as the test does not commit the project to keeping the Glock as a final game weapon. The pilot must prove that one native pistol behaviour can serve multiple definitions by changing only data, while unique mechanics remain implementable as separate native behaviours.

The first native behaviour slice covers definition lookup, native behaviour validation, primary-fire dispatch, semi-automatic press-edge handling, cooldowns, engine hitscan execution and fire presentation callbacks through the generic adapter. The generic entity now retains common runtime state across frames and saves/restores it, while preserving the existing GoldSrc clip and attack-timer transport. This is not yet the full pilot: controller-owned switching, general behaviour-specific state serialization, explicit prediction mapping for future custom state and in-game save/load verification still need implementation. The stock Glock must remain unchanged until its special timing, accuracy recovery and event presentation can be represented by the new architecture.


## Adjacent client HUD / VGUI direction

The HUD modernization is a sibling client-presentation subsystem on the same development branch. It is not part of the weapon-definition or weapon-behaviour runtime. Keep these responsibilities separate while allowing them to exchange read-only presentation data through narrow interfaces.

- Use the existing FreeVGUI/VGUI1 compatibility layer as the panel substrate; do not port VGUI2 wholesale or introduce a second GUI framework.
- Build a small client HUD layer with independent panels and lifecycle: health, suit battery, and current-weapon ammunition. Shared layout, typography, visibility and animation support belong in the HUD layer rather than in individual weapons.
- Health and suit power remain player-state presentation. The ammunition panel reads the active weapon's clip and reserve values through a narrow client-side compatibility/data interface; it must not depend on server weapon classes.
- The current AUX POWER meter is a separate, established HUD element. Its sprite sheet, draw code, layout cvars, editor, prediction source and fade behaviour must remain intact while adjacent HUD elements are modernized.
- Migrate incrementally with a default-off cvar and a legacy renderer fallback until each panel passes an in-game check.

### First HUD slice: health number

The first opt-in implementation adds a non-interactive VGUI label to the existing viewport. It reads `CHudHealth::m_iHealth`, reuses the existing health colour/fade calculation and original numeric anchor, and leaves damage/pain overlays on their existing path. With `hud_vgui 0` (default), the original cross/number/separator remains in use. With `hud_vgui 1`, VGUI draws the number in place of that sprite group. The AUX POWER meter call remains in its original position and is not reimplemented by VGUI. The health number now prefers the original OCR-B TrueType font at `resource/ocr-b.ttf`, rasterised by FreeVGUI's POSIX FreeType backend. The existing `resource/ocr-b-vgui.tga` bitmap atlas remains a compatibility fallback for builds without the FreeType backend. The selected font file must be present in the active game's `resource` directory.

**Validation state:** the health panel has been built, installed and confirmed in-game. The OCR-B atlas loaded and the health colour/fade behaved as expected; the AUX POWER meter remained unchanged.

### Second HUD slice: suit and primary ammunition

The next opt-in panels reuse the same OCR-B TrueType font, caption font and scoped additive VGUI text path. `hud_vgui 1` replaces only the suit battery number with a `SUIT` caption and OCR-B value; the existing suit charge graphic and battery fade remain on their original renderer path. `hud_vgui 1` replaces the primary weapon's clip/reserve digits with an `AMMO` caption and OCR-B row while retaining the weapon-specific ammo icon and existing fade. Weapons without a clip display reserve ammunition only. The special secondary-ammo row remains on its legacy path for this slice.

The single `hud_vgui` cvar defaults to `0` and controls all three panels. With `hud_vgui 0`, the original health, suit and ammo number rendering remains in use; with `hud_vgui 1`, all available VGUI panels are enabled together. The panels retain their own legacy fallback when VGUI is unavailable. The AUX POWER meter remains independent and untouched.

**Validation state:** suit/ammo source integration is committed; rebuild, install and in-game layout/fade testing are still required. The FreeType TrueType path is implemented but still needs a local build and in-game verification.
