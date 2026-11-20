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
| `hint_replace_key` | Ends other active map-authored hints using the same non-empty replacement key before this hint is displayed. |

Icon identifiers map to optional GoldSrc sprites under `valve/sprites/`: for example, `hint_icon_onscreen "icon_interact"` loads `sprites/icon_interact.spr`. Do not include the directory or `.spr` extension in the identifier. Source's original VGUI/VTF icon assets are not GoldSrc sprites, so the project must provide or convert the artwork. Missing sprites fall back to a text/arrow presentation. For a key prompt that changes to the interaction symbol outside the view, set `hint_icon_onscreen "use_binding"`, `hint_icon_offscreen "icon_interact"`, and `hint_binding "+use"`. The off-screen icon is selected when the target leaves the view; the 720-unit distance cutoff does not itself trigger that switch.

## Activation

Use the entity like a GoldSrc `func_button` target:

- `USE_ON`: show.
- `USE_OFF`: end.
- `USE_SET`: show when the value is greater than 0.5, otherwise end.
- `USE_TOGGLE`: toggle the current state.

Each active `env_instructor_hint` remains independently owned by its entity and recipient. The automatic player-use prompt is suppressed while any map-authored hint for that player is active; ending one hint does not release suppression while another remains active. This is derived from the live active-entity set, so a removed hint cannot leave a stale player-side owner ID. Map hints take priority over the automatic player-use prompt. An `env_instructor_hint` still requires its `Use` input to be fired; proximity alone does not activate that map entity. Hints are ended when their activator dies or their timeout expires.

## Automatic interaction guidance and pushable action prompt

The player's automatic interaction guidance has two stages. The near-field ready prompt uses the resolved `+use` keycap and `Interact` caption only for a visible use-capable entity selected by the original 64-unit forward-cone scan used for the PS2 brackets. When no entity is in that focused-use cone, a separate proximity-only approach scan searches visible use-capable entities within 256 units, rejecting candidates obscured by walls. During this approach stage, `icon_interact` is used both on-screen and off-screen; Source's locator renderer adds a directional arrow when the target is outside the title-safe view. When the entity enters the 64-unit forward-use cone, the same hint transitions to `use_binding` on-screen, `icon_interact` off-screen, and the `Interact` caption. The approach scan never creates a PS2 bracket and never extends actual USE focus or activation beyond the original 64-unit rule. The automatic hint uses `use_binding` as its on-screen icon and `icon_interact` as its off-screen icon. The `Interact` caption and key label use the normal GoldSrc HUD font, with their RGB intensity driven by the locator's shared fade value. The key label is centered over the keycap and fades from black toward the light keycap face. The automatic `use_binding` key sprite and `icon_interact` are both drawn at their native .spr dimensions (without the general 1.2 locator enlargement), so the icon swap does not change their base size. This automatic prompt is code-driven and does not require a map-authored instructor script. Pushables retain their separate move/hold prompt, selected from actual velocity changes in `CPushable::Move`; the action state decays back to the move prompt after 0.4 seconds without another successful impulse.

## Deliberate GoldSrc differences

- Map-authored hints and scripted managed lessons share the ten-target
  locator pool. Map hints keep independent lifecycles; the `C_GameInstructor`
  manager can present multiple eligible lessons tied at the highest selected
  display priority, subject to the shared pool capacity.
- Scripted lessons stop when they lose the active presentation slot. Their
  active start-time clock is reset, so timeout progression pauses while
  inactive unless `can_timeout_while_inactive` is enabled; `OnOpen` runs
  again when the lesson becomes active. Active and inactive updates validate
  target entity indices and reject `EF_NODRAW` targets unless
  `allow_nodraw_target` is enabled. GoldSrc does not expose Source's
  `EHANDLE`/dormancy semantics directly, so validation uses the client entity
  table and its available effect flags.
- Target-backed hints send position/visibility updates only when the target's encoded `WRITE_COORD` values change (1/8-unit coordinate precision), its target/visibility state changes, or its occlusion state changes. Target-following still ticks at 0.05 seconds, but visibility traces are cached and refreshed at 0.25-second intervals, matching Source's locator occlusion cadence. This avoids repeating identical network packets and expensive traces while preserving responsive moving-target updates.
- Target-backed hints use a projected world-space icon and a small caption tag while on-screen. Like Source's `ICON_DIST_TOO_FAR`, captions and ordinary inline binding labels are suppressed beyond 720 units (60 feet), measured from the current camera origin; the marker is also reduced to half size. For ordinary map-authored hints, this distance cutoff does not switch icons: when the target crosses Source's title-safe inset (7.5% of screen width) or moves behind the view, the renderer selects `hint_icon_offscreen` and places it on Source's 100-pixel (YRES-scaled) compass ellipse with a directional arrow. `hint_forcecaption` bypasses caption-distance and occlusion suppression, but not the separate `hint_range` culling rule. The automatic `-2` Interact prompt is limited by the server's 64-unit forward-cone use-target scan, so Source's 720-unit distance presentation threshold applies only to longer-range locator hints. Static hints keep a screen-space prompt and are not subject to this distance cutoff. The HUD uses GoldSrc sprite files and a custom user message rather than Source game events, VGUI locator panels, and VTF materials.
- Locator pulse/alpha/shake timing now follows Source's presentation equations: 1.0–1.75 pulse scale, one-second activation pulse, 0.5–1.0 alpha modulation, random ±2/±5 pixel shake, 0.3-second fades, and the 100-pixel (YRES-scaled) offscreen compass ellipse. GoldSrc still uses `.spr` assets and a different HUD renderer, so its rasterization and layout cannot be pixel-identical to Source's VGUI implementation.
- Controller-specific button localization is not included. Keyboard bindings can
  resolve through the locator's `use_binding` path; stock FWGS Xash3D falls back
  to the first matching key because its client API lacks alternate-key lookup.
- The existing PS2 brackets remain a separate world-space interaction marker and are not replaced by the Source locator. The automatic `Interact` keycap is a separate locator attached to the same currently focused usable entity.
- Keycap labels are drawn as compact 5x7 black glyphs blended over the original key sprite. Short bindings such as E use a 2x scale (10x14 pixels), avoiding the oversized 3x version. `FillRGBABlend` applies the same locator fade alpha, so the black text remains black at full opacity and fades with the keycap. The keycap sprite uses the same per-target fade value through the appended Xash3D `pfnSPR_SetAlpha` client callback. The paired `loonylynn/xash3d-fwgs` engine and this SDK must be rebuilt together; do not use the updated client DLL with an engine that lacks the callback.

The server entity's keyvalue names and base flag semantics are aligned with the publicly available Valve `CEnvInstructorHint` implementation. See the source comments in `dlls/player.cpp` for the Source-compatible flag values and field mapping.

## Public Source reference files

- Valve-authored server entity: [env_instructor_hint.cpp](https://github.com/InfoSmart/InSource/blob/master/game/server/env_instructor_hint.cpp)
- Source game instructor manager: [c_gameinstructor.cpp](https://github.com/InfoSmart/InSource/blob/master/game/client/c_gameinstructor.cpp)
- Source locator presentation: [hud_locator_target.cpp](https://github.com/InfoSmart/InSource/blob/master/game/client/hud_locator_target.cpp) and [hud_locator_target.h](https://github.com/InfoSmart/InSource/blob/master/game/client/hud_locator_target.h)

These public files are the reference for the entity keyvalues, locator effect bits, and presentation behavior adapted here.
