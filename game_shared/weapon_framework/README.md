# Weapon framework foundation

This folder starts the shared identity layer for the modular weapon system. It
is compiled into both the server and client game DLLs.

## What it does now

- Stores a weapon's stable textual name and GoldSrc entity classname.
- Registers existing weapons against their reserved numeric IDs.
- Assigns add-on weapons an available numeric ID.
- Looks weapons up by ID, textual name, or entity classname.

The current reference entry is the existing Glock (ID 2, classname
`weapon_9mmhandgun`), registered during both server and client initialization.
This exercises the same definition and numeric ID on both sides without
changing the original pistol implementation.

This is still a catalogue only. It does **not** spawn weapons, replace
`CBasePlayerWeapon`, parse JSON, execute scripts, or change prediction. Those
systems will be added after the identity contract is established.

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
extension is designed.

The registry does not enlarge `MAX_WEAPONS`, alter `pev->weapons`, or change
`WeaponList`, `CurWeapon`, or prediction data. Add-on weapons require a
framework-enabled client DLL and matching definitions on the server and client.

## Registration order

Each DLL owns its own registry instance. Add-on definitions must be registered
in the same deterministic order on both sides. The future package loader will
sort packages and weapon names before calling `RegisterAddon`.

## Naming

A stable textual name can look like `sample:weapon/pistol`; its GoldSrc entity
classname can be `weapon_sample_pistol`. Both names must be unique and fit in
63 characters.
