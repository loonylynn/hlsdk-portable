# Runtime Jolt ragdoll definitions (.jrag)

The server DLL loads the physics definition associated with an NPC model at runtime.
It derives the path by replacing the model's `.mdl` extension with `.jrag`.
For example:

- Model: `models/zombie.mdl`
- Physics: `models/zombie.jrag`

The runtime file is read through the engine's virtual filesystem and cached by path.
A missing or invalid file causes ragdoll setup to fail cleanly, leaving the existing
GoldSrc death-animation fallback in control. No model-specific hull or joint values
are compiled into `npc_physics.cpp`.

## Format v1

The file is UTF-8 text. Blank lines and text after `#` are ignored. Directives are
ordered and each directive occupies one line. Bone names must be double-quoted.

```text
JOLT_RAGDOLL 1
mass 115
inertia_multiplier 10
linear_damping 0.01
angular_damping 1.5
friction 0.8
restitution 0
self_collisions 1
body_count 1

body "Bip01 Pelvis" -1
center 1.2 1.4 0.0
joint_min -1e30 -1e30 -1e30
joint_max 1e30 1e30 1e30
angular_damping_override 0
hull 8
point -2 -5 -6
point 5 -3 -6
point 5 -3 6
point -2 -5 6
point -5 3 -6
point -5 3 6
point 2 5 6
point 2 5 -6
end_body

end
```

The example above illustrates the syntax only; the shipped zombie asset contains
the full 15-body rig.

### Properties

- `mass`: default total ragdoll mass. The optional server cvar
  `sv_npc_vphysics_mass` overrides it when set to a positive value; zero uses
  the asset value.
- `inertia_multiplier`, `linear_damping`, `angular_damping`: Jolt body settings.
- `friction`, `restitution`: material settings for all bodies.
- `self_collisions`: optional `0` or `1`; defaults to `1`. Without an explicit
  `collision_rules` block, enabled means every body pair can collide except
  directly constrained parent/child pairs. Set to `0` to disable all collisions
  between pieces of the same ragdoll, matching Source's `selfcollisions 0` rule.
- `collision_rules 1`: optional explicit body-pair allow-list, matching a
  Source `collisionrules` block. When present, all ragdoll body pairs start
  disabled and only pairs named by `collision_pair` are enabled. An empty list
  therefore disables all body-to-body collisions. This directive cannot be
  combined with `self_collisions 0`.
- `collision_pair A B`: repeatable pair of distinct, zero-based **body indices**
  from the `body_count` / `body` order. Pair order is irrelevant; duplicates and
  indices outside `body_count` are rejected. These are physical-body indices,
  not studio skeleton-bone indices.
- `animated_friction`: optional `minimum maximum time_in time_hold time_out`
  values. When present, the runtime ramps each angular joint's friction from
  minimum to maximum, holds it, then ramps it back down, matching Source's
  `animatedfriction` sequence. Friction values are Jolt angular torque limits;
  times are seconds. Omit the directive to leave animated joint friction off.
- `body_count`: number of physical bodies, 1–64.
- `body "name" parent`: exact studio bone name and parent body index. Use -1
  for the single root. Bodies must be listed parent-first.
- `center`: collision hull center offset in bone-local coordinates.
- `joint_min` and `joint_max`: angular limits in degrees (X, Y, Z). Use
  large finite values such as +/-1e30 for an effectively unlimited axis.
- `angular_damping_override`: per-body override; zero inherits global damping.
- `hull` and `point`: convex collision hull vertices in bone-local space;
  4–32 points are accepted.
- `end_body` and `end`: required terminators.

The generator enables Source's default self-collision behavior. Pass
`--no-self-collisions` to `jragtool generate` (or `smd2jrag.py`) to emit
`self_collisions 0`.

To author an explicit Source-style collision allow-list, pass
`--collision-rules` and repeat `--collision-pair BODY_A BODY_B` for every pair
to enable. For example:

```bash
python3 tools/jragtool.py generate model.qci --collision-pair 0 4 \
    --collision-pair 2 5 -o model.jrag
```

A pair option implies `--collision-rules`. Use `--collision-rules` by itself
for an empty allow-list, where no two ragdoll bodies collide. The pair indices
are the zero-based body indices printed by `jragtool inspect` / `generate`.

To author the Source-style animated joint-friction envelope, pass five values:
`--animated-friction MIN MAX TIME_IN TIME_HOLD TIME_OUT`. For example,
`--animated-friction 10 100 0.4 1.5 0.8` ramps from 10 to 100 over 0.4
seconds, holds for 1.5 seconds, then returns to 10 over 0.8 seconds. The
default is no animated joint friction.

Existing v1 files without these optional directives remain valid and retain
the original defaults.

The loader rejects invalid parent ordering, duplicate bone names, multiple roots,
missing hulls, out-of-range limits, non-finite values, trailing directives and
files larger than 1 MiB.

## Installing the shipped zombie rig

Copy `assets/valve/models/zombie.jrag` into the active game's `models/` folder.
For a typical installation:

```bash
GAME="/path/to/xash3d/valve"
install -Dm644 assets/valve/models/zombie.jrag "$GAME/models/zombie.jrag"
```

The file is loaded from the engine's active game search paths. It does not require
rebuilding the SDK when you edit physics values or hulls; restart the map/game so
the runtime cache is rebuilt. A map reset also discards the cached definitions.

The current format is a runtime definition, not a parser for Valve QC/QCI source.
The supplied zombie definition preserves the existing SMD/QCI-derived hulls,
bone hierarchy, joint limits and mass/damping values.

## Pose replication

Jolt simulation remains fixed-step at 120 Hz. Network pose snapshots are sent at
20 Hz while any body in a ragdoll is active. Once all bodies have entered Jolt's
sleeping state, the server sends a complete pose heartbeat at 5 Hz instead.
If a sleeping ragdoll wakes after an impact, it returns to the 20 Hz update rate.

The client interpolates using the actual interval between received samples,
rather than assuming every snapshot is exactly 50 ms apart. Its stale-pose
timeout allows for the slower sleeping heartbeat and occasional packet loss.
This changes network traffic only; it does not lower the Jolt simulation rate
or alter the physical behavior of the ragdoll.


## Low-violence behavior

The server cvar `sv_npc_vphysics_low_violence` defaults to `0`. Set it to `1`
to force the ordinary and important ragdoll limits to zero, releasing all
Jolt bodies and fading their frozen GoldSrc carriers using the configured
retirement fade. The mode is server-wide.

The standard GoldSrc violence cvars are honored independently. `violence_hblood`
and `violence_ablood` suppress attached impact marks for red and alien blood,
respectively. When `violence_hgibs` or `violence_agibs` is disabled, a death
that would otherwise gib uses GoldSrc's normal no-gibs corpse-fade path rather
than being handed to Jolt. Normal non-gib deaths still use the articulated
ragdoll system when global low-violence mode is off.

## Movable entity interactions

GoldSrc `func_pushable` entities remain under GoldSrc movement ownership and
are mirrored as kinematic Jolt boxes. Their motion transfers through the
kinematic surface velocity; a ragdoll impact applies the stock non-player
horizontal factor of 0.25 and respects the pushable's existing maximum speed.

Non-actor entities using `MOVETYPE_TOSS` or `MOVETYPE_BOUNCE` with
`SOLID_BBOX` or `SOLID_SLIDEBOX` are also mirrored as kinematic, axis-aligned
Jolt boxes when each half-extent is between 0.5 and 128 GoldSrc units. Their
GoldSrc movement transfers surface velocity to ragdolls. A ragdoll impact
queues one strongest normal-directed velocity increment per entity per server
frame; the increment is applied to GoldSrc `pev->velocity` on all three axes,
uses a 0.25 approach-speed factor, and is capped at 500 units/second.
Jolt callbacks never modify the entity directly.

GoldSrc remains authoritative for these entities' movement and collision
callbacks. This bridge does not synthesize angular velocity, rotated/custom
collision geometry, mass/inertia, or support for arbitrary movement types.
The pushable-specific response retains its horizontal speed limit and now
also transfers a bounded vertical component, clamped to the pushable's
configured maximum speed. The toss/bounce response supports vertical motion.

## Weapon impacts

Player and monster hitscan bullets and the crowbar query the Jolt collision world
against the articulated ragdoll bodies. A confirmed hit applies an impulse at the
actual hit point on the contacted body, so off-center hits can rotate the limb or
body as well as move it. The Jolt query is limited by the original GoldSrc trace
fraction and also sees the Jolt BSP/brush colliders, preventing impacts through
nearer world geometry. The hit position is returned to the weapon trace so bullet
tracers and hit handling stop at the ragdoll. Living NPCs continue using the
original GoldSrc damage and collision path.


### Weapon force model and attached impact marks

Bullet impulses use the same physical conversion used by the HL2 ammo definitions:
projectile mass in grains is converted to kilograms, muzzle velocity in feet per
second is converted to inches per second, and the Source 3.5 impulse exaggeration
factor is applied. The initial mappings are 200-grain / 1225-ft/s pistol and
SMG ammunition, 800-grain / 5000-ft/s .357 ammunition, and 400-grain / 1200-ft/s
shotgun buckshot. The 12 mm monster round uses a provisional 400-grain /
1225-ft/s mapping. All impulses are capped by the Jolt bridge to avoid extreme
velocities with the reduced zombie rig.

The crowbar follows Source's melee damage-force calculation:
base damage multiplied by a 75 kg target mass and a 4 in/s-per-damage scale.
Both paths apply their impulse at the actual Jolt contact point, wake the hit
body, and request an immediate pose snapshot.

Server tuning cvars (default 1.0):

- `sv_npc_vphysics_bullet_scale`: scales the Source-derived bullet impulse.
- `sv_npc_vphysics_crowbar_scale`: scales the Source-style crowbar impulse.

When a bullet or crowbar hits a bleeding Jolt ragdoll, the old GoldSrc reverse
`TraceBleed` trace is skipped. That trace ignored monsters and could pass through
the physics corpse to leave a decal on the floor or wall behind it. Blood
particles still spawn at the impact point.

The server sends the struck studio bone, mapped-bone-local hit point, and
local impact normal to clients. The hardware studio renderer transforms the current GoldSrc studio
triangle strips/fans with the same bone matrices used for the articulated
corpse, finds the nearest surface at the impact point, then projects and clips
a small irregular decal patch against the skinned mesh triangles. The clipped
fragments are cached in per-triangle barycentric coordinates: subsequent frames
only transform the small set of triangles touched by each mark, rather than
rescanning the whole model. The fragments are drawn at the actual mesh surface
with a minimal depth bias, rather than as a quad floating above the bone. This
keeps marks attached to moving ragdolls without repeated full-model projection
work or changes to the Xash3D engine. `cl_ragdoll_bloodmarks 0` disables
these marks. Set `cl_ragdoll_bloodmarks_debug 1` to log, once per mark,
how many studio triangles were parsed and how many decal fragments were
created. The client retains up to 32 recent marks per ragdoll.

This backports the core studio-decal projection and polygon-clipping behavior
used by Source, not Source's entire `IVModelRender`/model-instance decal
subsystem. Decals are currently limited to the hardware studio renderer and
the existing hitscan bullet/crowbar impact hooks; projectiles and explosive
radius impacts are not yet wired into the Jolt ragdoll path.
