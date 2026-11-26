# Weapon framework foundation

This folder contains the shared identity, metadata, and package-catalogue layer
for the modular weapon system. It is compiled into both the server and client
game DLLs.

## What it does now

- Stores a weapon's stable textual name and GoldSrc entity classname.
- Registers existing weapons against their reserved numeric IDs.
- Assigns add-on weapons an available numeric ID.
- Looks weapons up by ID, textual name, or entity classname.
- Loads a root package catalogue, independent package manifests, and their
  weapon-definition JSON files through the game virtual filesystem.
- Sorts packages and the weapons inside each package by stable name before
  assigning add-on IDs.
- Publishes the catalogue atomically, so a failed package cannot leave a
  partially registered set of weapons.

The current reference entry is the existing Glock (ID 2, classname
`weapon_9mmhandgun`). It is listed in the `goldsrc:base` package and loaded
through the same catalogue path used by future add-ons. This exercises the
same definition and numeric ID on both sides without changing the original
pistol implementation.

The shared layer stores identity and gameplay metadata and reads version-1
JSON definitions. It still does **not** spawn arbitrary new weapon entities,
replace `CBasePlayerWeapon`, execute scripts, or change prediction. Those
capabilities will be built on top of this definition and package layer.

## Numeric ID compatibility

The current game code uses fixed-size weapon arrays and GoldSrc's existing
weapon messages/bitset. This foundation keeps that layout unchanged:

| IDs | Purpose |
| --- | --- |
| 0 | No weapon |
| 1-16 | Existing built-in/legacy weapon IDs |
| 17-30 | Available add-on weapon IDs |
| 31 | HEV suit bit |

There is room for **14 add-on weapons at once**. A package catalogue may contain
more definitions, but only 14 can have live IDs until a negotiated protocol
extension is designed. The current loader accepts up to 32 packages and up to
32 definition entries per package; those are catalogue limits, not extra live
weapon slots.

The registry does not enlarge `MAX_WEAPONS`, alter `pev->weapons`, or change
`WeaponList`, `CurWeapon`, or prediction data. Add-on weapons require a
framework-enabled client DLL and matching catalogue/package files on the
server and client.

## Package catalogue and load order

The root index is `scripts/weapons/packages.json`. Each entry supplies a
stable package name and the virtual-filesystem path to that package's manifest.
The base SDK includes `scripts/weapons/package_goldsrc.json`, which lists the
Glock definition.

A third-party package can keep its manifest and definitions inside its own
PK3. To make the package visible to the framework, add an entry to the root
catalogue whose `manifest` path points to that file. The package manifest
declares its own package name and lists weapon stable names with definition
file paths. Each listed name must match the name inside the referenced weapon
JSON.

For example, a package manifest can point to a definition under
`addons/sample_pack/weapons/pistol.json`. The path is resolved by the game
virtual filesystem, so the file can come from a mounted PK3 rather than a loose
file on disk. The current system deliberately uses an explicit catalogue; it
does not scan arbitrary PK3 directories.

Package names are sorted lexically first. Weapon names are then sorted
lexically within each package. The same package catalogue on client and server
therefore assigns add-on IDs in the same order, regardless of the order of
entries in the JSON files. Duplicate package names/paths, duplicate weapon
names/paths, duplicate entity classnames, invalid paths, and unsupported JSON
fields are rejected.

If the catalogue, a package manifest, or any referenced weapon file is missing
or invalid, the entire staged catalogue is discarded. Both DLLs then register
the same built-in Glock fallback. This is intentionally strict: partial
client/server catalogues could assign different numeric IDs and break
prediction or inventory messages.

The schemas for the three JSON layers are:

- `schema/package_catalog.schema.json`
- `schema/package_manifest.schema.json`
- `schema/weapon.schema.json`

## Naming

A stable weapon name can look like `sample:weapon/pistol`; its GoldSrc entity
classname can be `weapon_sample_pistol`. Both names must be unique and fit in
63 characters. Package names use the same namespaced format.

## Weapon definition contract (version 1)

The example in `examples/weapon_glock.json` records the metadata currently
returned by `CGlock::GetItemInfo()` and the existing deploy model paths. Its
runtime copy is `scripts/weapons/weapon_glock.json`.

The loader rejects unsupported versions, unknown/duplicate fields, invalid
value types, invalid model paths, and values outside the schema's bounds.
Weapon firing, animation sequences, and prediction remain in the existing
native code.

## Metadata used by the reference weapon

The Glock is still implemented by its original native class, but its item
metadata (slot, position, weight, flags, clip size, ammo types/capacities),
starting ammo, pickup amount, and model paths are supplied through the shared
registry. The map ammo pickup grants the configured ammo type, amount, and carry
limit. Clip size is used by the native reload path, not merely displayed to the
HUD.

The C++ built-in definition remains only as a fallback when the package
catalogue or one of its files is absent or fails validation.

## Runtime file loading

The server reads catalogue and package files through the GoldSrc
`LOAD_FILE_FOR_ME` callback. The client uses its `COM_LoadFile` callback.
Both call the same allocation-free parser and release each engine-owned file
buffer after parsing. The package loader builds a temporary registry and only
replaces the live registry when every package has loaded successfully.

The root build scripts install the JSON files in `scripts/weapons/` into the
selected game directory. Third-party packages can distribute their own JSON
files in PK3s, provided their manifest paths are listed in the root catalogue.


## Focused tests

The shared parser and package registry have a standalone test target that does
not require the engine or a 32-bit SDK build. With CMake and a C++ compiler
installed, run:

\`\`\`sh
cmake -S game_shared/weapon_framework/tests -B build-weapon-framework-tests
cmake --build build-weapon-framework-tests
ctest --test-dir build-weapon-framework-tests --output-on-failure
\`\`\`

The tests cover valid and invalid weapon definitions, duplicate identity
rejection, deterministic add-on IDs when package and weapon arrays are
reordered, and preservation of the last valid registry when a catalogue load
fails. They validate the metadata layer only; they do not exercise GoldSrc
entity spawning, firing, or client prediction.
