# Authoring Jolt ragdolls from SMD/QC/QCI

```text
Model QC + recursive $include chain + collision SMD
                    |
                    v
             tools/smd2jrag.py
                    |
                    v
              modelname.jrag
                    |
                    v
       Xash3D server loads it at runtime
```

The converter lives in `tools/smd2jrag.py`. It uses only Python's standard
library; it does not need Blender, `studiomdl`, SciPy, or a rebuild of the SDK.

## Convert the existing zombie ragdoll

The converter accepts either a model `.qc` or a standalone `.qci`.
When given a model QC, it follows nested `$include "file.qci"` directives
recursively until it finds the single `$collisionjoints` block. Include paths
are resolved relative to the file containing each directive. The collision SMD
is then resolved relative to the QC/QCI file containing that block.

For the current source, `$collisionjoints ragdoll` resolves to `ragdoll.smd`
beside `ragdoll.qci`. A quoted SMD path such as
`$collisionjoints "collision/ragdoll.smd"` is also accepted. The `.smd`
extension is not appended twice, so `--smd` is optional:

```bash
cd ~/hlsdk-portable

python3 tools/smd2jrag.py /path/to/model/model.qc \
    --output assets/valve/models/zombie.jrag
```

A standalone QCI invocation remains supported:

```bash
python3 tools/smd2jrag.py /path/to/private/ragdoll.qci \
    -o assets/valve/models/zombie.jrag
```

Or specify the SMD explicitly:

```bash
python3 tools/smd2jrag.py /path/to/private/ragdoll.qci \
    --smd /path/to/private/ragdoll.smd \
    --output assets/valve/models/zombie.jrag
```

Expected summary for the supplied zombie sources: 15 rigid bodies, each with
8 hull points. Body parentage, local hull offsets, joint limits, total mass,
inertia multiplier and damping come from the SMD/QCI pair.

Copy the generated runtime file into the game's model folder. No SDK or engine
rebuild is required when only the `.jrag` contents change:

```bash
GAME="/path/to/xash3d/valve"
install -Dm644 assets/valve/models/zombie.jrag "$GAME/models/zombie.jrag"
```

Restart the map (or game) so the server's per-map definition cache is cleared.

## Adding another NPC

The runtime handoff is model-driven, not class-name-driven. For a monster using
`models/creatures/headcrab.mdl`, place its definition at
`models/creatures/headcrab.jrag`. The shared `CBaseMonster::Killed` path
attempts Jolt handoff for non-gib monster deaths; it no longer checks for the
`monster_zombie` classname.

A valid definition must map every authored physical bone to the model's studio
skeleton and preserve the nearest physical-ancestor hierarchy. If the sidecar is
missing, malformed, or incompatible with that model, Jolt does not take over:
the original GoldSrc death animation/corpse behavior remains in control.
Living NPCs continue to use GoldSrc AI, movement, and collision. Jolt only owns
the entity after a successful death-time handoff.

For diagnosis, the server prints a `[NPC physics] Loaded '...'` message when a
sidecar is first loaded. A missing or invalid definition is reported when that
NPC dies, and the failure is cached for the rest of the map.

## What the converter reads

From the SMD it reads the node hierarchy, the first reference skeleton frame
(prefer `time 0`; otherwise use the lowest available frame), and all triangle
vertices grouped by their assigned bone. SMD Euler rotations are composed as
`Rz * Ry * Rx`. Vertices are transformed from model space into each collision
bone's local space before the center offset and hull points are written.

From the resolved QCI `$collisionjoints` block it reads:

- `$mass`, `$inertia`, `$damping`, and `$rotdamping`
- `$rootbone`
- `$jointconstrain "bone" <x|y|z> limit <min> <max>`
- `$jointrotdamping "bone" value`

For each physical bone, the converter finds its nearest ancestor that also has
collision geometry. This preserves the physical hierarchy even when helper,
finger, neck, or other non-physical studio bones sit between rigid bodies.
`$rootbone` may be a non-physical skeleton anchor above the first physical body.

Valve QCI does not specify the Jolt material coefficients used by this bridge.
The converter therefore defaults to friction `0.8` and restitution `0`, matching
the currently shipped zombie asset. Override them with `--friction` and
`--restitution` when needed.

## Collision mesh requirements

Each physical bone must have a closed, three-dimensional convex collision
mesh assigned to that bone in the SMD. Duplicate triangle vertices are merged.
The resulting unique point cloud must contain 4–32 points and must not be
coplanar. Jolt builds the convex hull from those points at runtime.

The current `.jrag` loader accepts at most 32 points per body. If a body exceeds
that limit, simplify its collision mesh in the modelling tool and export the
SMD again. The converter deliberately fails with the bone name and point count
instead of silently dropping vertices or changing the shape.

Every QCI-constrained bone must have collision geometry in the SMD. Bone names
are matched case-insensitively, but the exact SMD spelling is kept in the
output. At present the runtime definition format limits body names to 64 bytes;
keep authored names within that limit.

The converter validates the single-root hierarchy, parent ordering, missing
bones, non-finite values, invalid joint ranges and degenerate hulls before
writing the file.

## Validation

Run the unit tests with:

```bash
python3 -m unittest discover -s tools/tests -v
```

To inspect the generated file and its rigid-body ordering:

```bash
less assets/valve/models/zombie.jrag
```

The output is the Xash3D/Jolt runtime `.jrag` format documented in
`docs/jolt-ragdoll-format.md`. It is not Valve's compiled `.phy` format and it
does not replace `studiomdl`; it converts the relevant SMD/QCI authoring data
for this project's runtime loader.
