# Weapon Framework In-Game Integration Test

This is a temporary integration fixture for the native `firearm.hitscan` behaviour and the generic `CFrameworkWeapon` entity. It is not a replacement for the stock Glock and does not change the stock weapon implementation.

## Test weapon

- Stable name: `xash:weapon/test_pistol`
- Give classname: `weapon_framework_test_pistol`
- Runtime entity: `weapon_framework`
- Behaviour: `firearm.hitscan`, semi-automatic
- Ammo: 9mm; 8-round clip; 24 starting rounds (expected first pickup: 8 loaded and 16 in reserve)
- Existing assets: Glock view/world/player models and `weapons/pl_gun3.wav`
- Glock model sequence indices: draw 7, fire 3, reload 5, idle 0
- No stock Glock class, definition, damage tuning, input handling or event code is migrated by this fixture.

## Before testing

1. Build and install both the server and client DLLs from the same feature-branch revision.
2. Install the updated `scripts/weapons/package_xash.json` and `scripts/weapons/weapon_framework_test_pistol.json` under the active game's `valve/scripts/weapons/` directory.
3. Start a disposable test map/session. Do not overwrite an important campaign save.
4. Set `developer 2` and `sv_cheats 1`.
5. Check the server console for `WeaponFramework: server package catalogue loaded successfully`. If it reports fallback to built-in Glock metadata, the server did not load the package catalogue; stop and fix the VFS path before continuing.

## Test sequence

### Acquisition and firing

1. Run `give weapon_framework_test_pistol`.
2. Confirm the pistol is selected and the Glock viewmodel is visible.
3. Confirm the clip starts at 8 and the 9mm reserve is 16 (subject to any ammo already carried).
4. Fire two separate taps at a safe wall. Confirm one round is consumed per press and the clip becomes 6.
5. Hold primary fire without releasing. Confirm the semi-automatic behaviour does not repeatedly fire as the cooldown expires.
6. Release, then press again. Confirm one more round is consumed and the weapon fires again.

### Active weapon save/restore

1. With the test pistol active and its clip no longer full, run `save wf_test_active`.
2. Run `load wf_test_active`.
3. Confirm the test pistol remains equipped and its clip/reserve counts are unchanged.
4. Fire once and confirm exactly one additional round is consumed.

### Holstered weapon save/restore

1. Make sure you own another weapon, then select it with the normal weapon-selection controls so the test pistol is definitely holstered.
2. Run `save wf_test_holstered`, then `load wf_test_holstered`.
3. Select the test pistol again.
4. Confirm its clip and reserve counts are unchanged and it can fire normally.

### Reload save/restore

1. Fire a few rounds so the clip is not full and confirm reserve ammo remains.
2. Press reload, then immediately run `save wf_test_reload` while the reload is still in progress.
3. Run `load wf_test_reload` and allow at least 1.5 seconds for the reload to finish.
4. Confirm the clip refills from reserve exactly once, reserve decreases by the amount transferred, and firing works afterward.

## What to report

For each case, record pass/fail and any console errors. A screenshot of the HUD before and after loading is useful, but the exact console message and reproduction steps are more important.

The old-save compatibility fallback is a separate case: it requires a save containing a generic framework weapon made before the custom `CFrameworkWeapon` runtime field block was introduced. Do not treat a normal save created by this test run as an old-save test.

## Current limits

The test proves the current generic hitscan entity's shared runtime fields and engine save/restore path only. The keyed archive for future behaviour-specific custom state is not connected to engine save/restore yet, and future custom prediction state still needs explicit transport. A successful test here does not establish those later capabilities.
