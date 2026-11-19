# GoldSrc `env_instructor_hint` backport

This is a GoldSrc/Xash3D single-player port of the public Source `CEnvInstructorHint` entity contract and a subset of the Source locator presentation rules. Target-following hints render as compact world-space locators; static hints remain screen-space prompts. The Half-Life user-message transport and existing PS2-style world-space use brackets remain independent.

## Supported keyvalues

| Key | Behavior |
|---|---|
| `hint_target` | Targetname followed by the hint. If omitted on a non-static hint, the activator/player is followed. |
| `hint_caption` | Main caption. |
| `hint_activator_caption` | Alternate caption used for the activating player when non-empty. |
| `hint_binding` | Literal binding text displayed with the caption. |
| `hint_timeout` | Seconds before the hint closes; 0 means no timeout. |
| `hint_icon_onscreen` | Optional icon identifier used while its target is on-screen. |
| `hint_icon_offscreen` | Optional icon identifier used while its target is off-screen. |
| `hint_color` | RGB value, accepted as `R G B` or `R,G,B`. |
| `hint_icon_offset` | Vertical world-space offset applied to the target point. |
| `hint_range` | Maximum distance from the player; 0 disables range culling. |
| `hint_pulseoption` | 0 = none, 1 = slow, 2 = fast, 3 = urgent. |
| `hint_alphaoption` | 0 = none, 1 = slow, 2 = fast, 3 = urgent. |
| `hint_shakeoption` | 0 = none, 1 = narrow, 2 = wide. |
| `hint_static` | Displays the card at a fixed HUD position rather than following a world target. |
| `hint_nooffscreen` | Hides the hint when its world target leaves the viewport. |
| `hint_forcecaption` | Bypasses Source-style caption distance and occlusion suppression. It does not bypass `hint_range` target culling. |
| `hint_allow_nodraw_target` | Allows a target with `EF_NODRAW` to remain a valid hint target. |
| `hint_local_player_only` | Retained for map compatibility; the project is single-player, so there is only one recipient. |
| `hint_replace_key` | Retained in the hint payload. The current single-panel HUD replaces its currently displayed hint when another hint starts. |

Icon identifiers map to optional GoldSrc sprites under `valve/sprites/`: for example, `hint_icon_onscreen "icon_interact"` loads `sprites/icon_interact.spr`. Do not include the directory or `.spr` extension in the identifier. Source's original VGUI/VTF icon assets are not GoldSrc sprites, so the project must provide or convert the artwork. Missing sprites fall back to a text/arrow presentation.

## Activation

Use the entity like a GoldSrc `func_button` target:

- `USE_ON`: show.
- `USE_OFF`: end.
- `USE_SET`: show when the value is greater than 0.5, otherwise end.
- `USE_TOGGLE`: toggle the current state.

The entity ID owns the active instructor presentation. A map hint takes priority over the automatic `func_pushable` prompt. Hints are ended when their activator dies or their timeout expires.

## Pushable action prompt

The move/hold icon is now selected from actual pushable velocity changes in `CPushable::Move`, not merely from the player holding USE. A successful `push` impulse selects move; a successful pull impulse selects hold. The action state decays back to the move prompt after 0.4 seconds without another successful impulse. The optional action art remains `sprites/icon_move.spr` and `sprites/icon_hold.spr`.

## Deliberate GoldSrc differences

- Up to ten map-authored `env_instructor_hint` instances can be displayed at
  the same time, each with independent lifecycle and presentation state. The
  separate scripted `C_GameInstructor` manager still selects one prioritized
  managed lesson for its HUD slot; concurrent managed-lesson presentation is
  not part of this stage.
- Target-backed hints use a projected world-space icon and a small caption tag while on-screen. Like Source's `ICON_DIST_TOO_FAR`, captions and ordinary inline binding labels are suppressed beyond 720 units (60 feet), measured from the current camera origin; the locator icon remains visible. `hint_forcecaption` bypasses this caption-distance cutoff and occlusion suppression, but not the separate `hint_range` culling rule. When off-screen, captions and normal inline binding labels are hidden; the off-screen icon and directional arrow remain. The Source `use_binding` virtual icon keeps its special behavior: its on-screen key glyph follows the caption-distance gate, while its off-screen key glyph remains available through the equivalent of `DrawBindingNameOffscreen`. Static hints keep a screen-space prompt and are not subject to this distance cutoff. The HUD uses GoldSrc sprite files and a custom user message rather than Source game events, VGUI locator panels, and VTF materials.
- Locator pulse/alpha/shake flags are approximated with GoldSrc HUD drawing; the halo and movement are not pixel-identical to Source's animation curves, and Source's VGUI layout/decluttering behavior is not reproduced.
- Controller-specific button localization is not included. Keyboard bindings can
  resolve through the locator's `use_binding` path; stock FWGS Xash3D falls back
  to the first matching key because its client API lacks alternate-key lookup.
- The existing PS2 brackets remain a separate world-space interaction marker and are not replaced by the Source locator.

The server entity's keyvalue names and base flag semantics are aligned with the publicly available Valve `CEnvInstructorHint` implementation. See the source comments in `dlls/player.cpp` for the Source-compatible flag values and field mapping.

## Public Source reference files

- Valve-authored server entity: [env_instructor_hint.cpp](https://github.com/InfoSmart/InSource/blob/master/game/server/env_instructor_hint.cpp)
- Source game instructor manager: [c_gameinstructor.cpp](https://github.com/InfoSmart/InSource/blob/master/game/client/c_gameinstructor.cpp)
- Source locator presentation: [hud_locator_target.cpp](https://github.com/InfoSmart/InSource/blob/master/game/client/hud_locator_target.cpp) and [hud_locator_target.h](https://github.com/InfoSmart/InSource/blob/master/game/client/hud_locator_target.h)

These public files are the reference for the entity keyvalues, locator effect bits, and presentation behavior adapted here.
