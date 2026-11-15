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
