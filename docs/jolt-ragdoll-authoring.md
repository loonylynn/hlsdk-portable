# Jolt ragdoll authoring tools

\`tools/jragtool.py\` is the command-line front end for authoring the project's
runtime \`.jrag\` files. It uses only the Python standard library. It does not
need Blender, \`studiomdl\`, SciPy, or an SDK rebuild.

The tool currently has three commands:

- \`inspect\`: inspect a Valve collision SMD's skeleton, physical-bone meshes,
  parent relationships, and (optionally) its QC/QCI ragdoll settings.
- \`generate\`: convert the SMD and QC/QCI \`$collisionjoints\` data into a
  runtime \`.jrag\` file, then validate the result before reporting success.
- \`validate\`: read a \`.jrag\` using the same structural rules as the runtime
  loader and compare its hull geometry and hierarchy against the source SMD.
  Passing \`--qci\` also verifies the mass, inertia, damping and joint limits.

## Scientist example

The Scientist source set contains \`ragdoll.smd\` and \`ragdoll.qci\`. The QCI
declares \`$collisionjoints ragdoll\`, so the converter resolves the matching
SMD in the same directory automatically.

\`\`\`bash
cd ~/hlsdk-portable

SCIENTIST="/media/lucasg/wii_gamecube hdd/Scientist"

# 1. Inspect the skeleton and collision meshes before generating anything.
python3 tools/jragtool.py inspect "$SCIENTIST/ragdoll.smd" \
    --qci "$SCIENTIST/ragdoll.qci"

# 2. Generate the Scientist runtime sidecar.
python3 tools/jragtool.py generate "$SCIENTIST/ragdoll.qci" \
    --output "$SCIENTIST/scientist.jrag"

# 3. Validate the generated file against both authoring sources.
python3 tools/jragtool.py validate "$SCIENTIST/scientist.jrag" \
    "$SCIENTIST/ragdoll.smd" --qci "$SCIENTIST/ragdoll.qci"
\`\`\`

Install the validated sidecar next to the compiled model in the active game:

\`\`\`bash
ENGINE_ROOT="/media/lucasg/wii_gamecube hdd/HALF-LIFE PROJECT/xash3d-fwgs-linux-i386"
GAME="$ENGINE_ROOT/valve"
install -Dm644 "$SCIENTIST/scientist.jrag" "$GAME/models/scientist.jrag"
\`\`\`

Restart the map (or game) so the server's per-map definition cache is cleared.
Changing only a \`.jrag\` file does not require rebuilding the SDK.

## Command reference

### Inspect an SMD

\`\`\`bash
python3 tools/jragtool.py inspect path/to/ragdoll.smd
\`\`\`

Inspect also accepts an optional QC/QCI source. This adds the resolved
\`$collisionjoints\` block, root bone, global physical properties, and the
resulting physical-body hierarchy:

\`\`\`bash
python3 tools/jragtool.py inspect path/to/ragdoll.smd --qci path/to/model.qc
\`\`\`

The report distinguishes all skeleton bones from collision bodies. For every
physical bone it lists its nearest physical ancestor, raw vertex count, unique
point count, and whether its point cloud satisfies the runtime's 4–32 point
three-dimensional hull requirements.

### Generate a runtime definition

\`\`\`bash
python3 tools/jragtool.py generate path/to/model.qc -o path/to/model.jrag
\`\`\`

The input can be a model QC or a standalone QCI. Nested \`$include\` directives
are followed recursively, and include paths are resolved relative to the file
containing each directive. The collision SMD path is resolved relative to the
file containing the \`$collisionjoints\` block. Use \`--smd\` to override it.

Valve QCI does not define Jolt friction or restitution, so the tool defaults to
friction \`0.8\` and restitution \`0\`. Set them explicitly when needed:

\`\`\`bash
python3 tools/jragtool.py generate path/to/ragdoll.qci \
    --friction 0.8 --restitution 0 -o path/to/model.jrag
\`\`\`

Generation is not considered successful until the written file has been read
back and checked against its source SMD/QCI.

### Validate an existing definition

\`\`\`bash
python3 tools/jragtool.py validate path/to/model.jrag path/to/ragdoll.smd
\`\`\`

This checks the runtime grammar and limits, exact physical-bone coverage,
nearest-physical-ancestor links, deterministic body ordering, local hull
centers, and generated hull points. To additionally compare the authored
settings with their QC/QCI source:

\`\`\`bash
python3 tools/jragtool.py validate path/to/model.jrag path/to/ragdoll.smd \
    --qci path/to/model.qc
\`\`\`

Validation without \`--qci\` still verifies the hull geometry and hierarchy;
the global and joint values are checked for legal ranges but cannot be compared
to their original authoring directives.

## How generation works

From the SMD, the converter reads the node hierarchy, a reference skeleton
frame (prefer \`time 0\`; otherwise use the lowest available frame), and triangle
vertices grouped by their assigned bone. SMD Euler rotations are composed as
\`Rz * Ry * Rx\`. Vertices are transformed from model space into each collision
bone's local space before the center offset and hull points are written.

From the resolved QCI \`$collisionjoints\` block it reads:

- \`$mass\`, \`$inertia\`, \`$damping\`, and \`$rotdamping\`
- \`$rootbone\`
- \`$jointconstrain "bone" <x|y|z> limit <min> <max>\`
- \`$jointrotdamping "bone" value\`

For each physical bone, the converter finds the nearest ancestor that also has
collision geometry. This preserves the physical hierarchy when helper, finger,
neck, or other non-physical studio bones sit between rigid bodies.
\`$rootbone\` may be a non-physical skeleton anchor above the first physical
body.

The generated hull point cloud is the unique set of SMD collision-mesh vertices
assigned to each bone, transformed into bone-local space. Each physical bone
must have a closed, three-dimensional convex collision mesh. Duplicate triangle
vertices are merged. The point cloud must contain 4–32 unique vertices and
must not be coplanar. The tool fails with the offending bone name rather than
silently dropping vertices or truncating the collision shape.

The runtime format currently limits definitions to 64 physical bodies, 32 hull
points per body, and 64 UTF-8 bytes per bone name. Bone names are matched
case-insensitively while preserving their original spelling in the output.

## Tests

Run the converter and CLI test suites with:

\`\`\`bash
python3 -m py_compile tools/smd2jrag.py tools/jragtool.py
python3 -m unittest discover -s tools/tests -v
\`\`\`

\`tools/smd2jrag.py\` remains available as the original generate-only converter
for existing scripts. New authoring workflows should use \`tools/jragtool.py\`.
The generated file is this project's Jolt runtime format, documented in
[jolt-ragdoll-format.md](jolt-ragdoll-format.md). It is not Valve's compiled
\`.phy\` format and does not replace \`studiomdl\`.
