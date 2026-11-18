# GoldSrc Game Instructor lesson manager

This is a GoldSrc/Xash3D adaptation of Valve's public Source `C_GameInstructor`,
`CBaseLesson`, and `CScriptedIconLesson` architecture. It is not a byte-for-byte
Source port: Source engine events, entity handles, KeyValues, VGUI2, locator
handles, and save systems do not exist in this client DLL. The adapted manager
keeps the lesson-definition/runtime-opportunity model and maps engine-facing
parts to GoldSrc.

## Script files and load order

The client loads:

1. `valve/scripts/mod_lessons.txt`
2. `valve/scripts/instructor_lessons.txt`

Mod lessons are read first; a lesson name already defined there wins over the
same name in the common file. Both files use Source-style KeyValues syntax.
The repository includes empty manifests so the feature is inert until lessons
are added.

A lesson definition contains settings and event rules. Event sections are
`open`, `close`, `success`, `onopen`, and `update`. Rules are evaluated in
order. A failed test aborts the current rule; `scope in` / `scope out` provide
conditional groups in the same way as Source lesson scripts.

## Example: teach the player to fire

Add this inside the top-level object in `instructor_lessons.txt`:

```text
"Basic weapon fire"
{
    "instance_type" "1"
    "priority" "80"
    "timeout" "20"
    "success_limit" "1"
    "caption" "Press attack to fire"
    "binding" "+attack"
    "no_icon_target" "1"
    "fixed_position" "1"
    "fixed_position_x" "0.5"
    "fixed_position_y" "0.75"

    "open"
    {
        "instructor_primaryattack"
        {
            "local_player is" "player userid"
        }
    }

    "success"
    {
        "use_primaryattack"
        {
            "local_player is" "player userid"
            "void close" "void"
        }
    }
}
```

This example deliberately uses a fixed-position screen prompt and needs no
world target or icon sprite. Target-backed lessons use the world-space locator
renderer described in the next example. To supply actual icon artwork, provide
a GoldSrc sprite as `valve/sprites/<icon-name>.spr` and use its basename in
`onscreen_icon` / `offscreen_icon`.

## Example: locate the object the player uses

This lesson captures the entity passed by the `player_use` event and attaches
the locator to that entity. It is useful as a smoke test: look at an object that
accepts USE and press the USE key.

```text
"Locate used object"
{
    "instance_type" "1"
    "priority" "110"
    "timeout" "8"
    "caption" "You interacted with this"
    "binding" "+use"
    "onscreen_icon" "icon_interact"
    "offscreen_icon" "icon_arrow_plain"
    "icon_offset" "28"
    "range" "1024"

    "open"
    {
        "player_use"
        {
            "local_player get use target" "int 1"
            "icon_target set" "entity entity1"
        }
    }
}
```

A target-backed lesson now draws a small icon at the target's projected
world position, with a compact caption tag that follows it. When the target
leaves the viewport, the off-screen icon is placed at the nearest screen edge;
`no_offscreen` suppresses it. The `fixed_position` and `no_icon_target` forms remain
screen-space prompts rather than world locators. `icon_interact` and
`icon_arrow_plain` are Source icon names: this port expects matching `.spr`
files under `valve/sprites/`. Without those converted assets, a simple
reticle/directional-arrow fallback is drawn.

## Lesson properties

Implemented properties include:

- Instance policy: `instance_type` (0 multiple, 1 single-open, 2 fixed-replace,
  3 single-active), `replace_key`, `fixed_instances_max`,
  `replace_only_when_stopped`.
- Scheduling and lifetime: `priority`, `timeout`, `lock_duration`,
  `update_interval`, `display_limit`, `success_limit`,
  `can_open_when_dead`, `bump_with_timeout_when_learned`,
  `can_timeout_while_inactive`.
- Lesson grouping: `group`, repeated `prereq` entries, and
  `GroupConVarToggle` with `convar` and `group` fields.
- Text and locator presentation: `caption`, `caption_param`, `binding`,
  `gamepad_binding`, `onscreen_icon`, `offscreen_icon`, `caption_color`,
  `range`, `icon_offset`, `flags`, `no_icon_target`,
  `allow_nodraw_target`, `visible`, `no_offscreen`, `force_caption`,
  `fixed_position`, `fixed_position_x`, and `fixed_position_y`.

The following Source metadata is accepted but is not rendered by the GoldSrc
HUD: `vgui_target_name`, `vgui_target_lookup`, `vgui_target_edge`,
`icon_relative_offset`, and `show_when_occluded`. The HUD has no equivalent
VGUI target anchors or full Source-style target occlusion trace.

Lesson variables exposed to rules are:
`local_player`, `entity1`, `entity2`, `icon_target`, `integer1`,
`integer2`, `output`, `float1`, `float2`, `string1`, `string2`,
`caption`, `caption_param`, `binding`, `gamepad_binding`,
`onscreen_icon`, `offscreen_icon`, `replace_key`, `group`, `range`,
`timeout`, `priority`, `flags`, `visible`, `no_offscreen`,
`force_caption`, `no_icon_target`, `allow_nodraw_target`,
`show_when_occluded`, `fixed_position`, `fixed_position_x`,
`fixed_position_y`, `icon_offset`, `icon_relative_offset`,
`caption_color`, `start_sound`, `vgui_target_name`,
`vgui_target_lookup`, `vgui_target_edge`, `lock_duration`,
`update_interval`, `can_open_when_dead`,
`bump_with_timeout_when_learned`, and `can_timeout_while_inactive`.

## Rule actions and parameter types

The adapted evaluator supports:

- Control: `scope in`, `scope out`, `close`, `success`, `lock`,
  `present complete`, `present start`, `present end`, and `reference open`.
- Values: `set`, `add`, `subtract`, `multiply`, `is`, `less than`,
  `has prefix`, `has bit`, `bit count is`, `bit count less than`,
  `get distance`, `get angular distance`, `classname is`,
  `modelname is`, `get use target`, and `use target is`.
- Parameter types: `void`, `string`, `bool`, `float`, `int`, `integer`,
  `short`, `long`, `player`, `entity`, and `convar`.
- Prefix an action with `?` to make an unavailable optional parameter non-fatal;
  prefix it with `!` to invert a comparison.

`get distance` and `get angular distance` write their result to `output`.
`classname is` can compare the entity carried by the current event;
`modelname is` compares a client entity's model path. `get use target` and
`use target is` are supported when processing the `player_use` event.

The client-side archived counters are stored as
`gameinstructor_<lesson>_display` and
`gameinstructor_<lesson>_success` cvars. Set
`gameinstructor_enable 0` to disable lesson scheduling.

## GoldSrc gameplay events currently published

The server-to-client `InstructorEvent` message currently emits:

- `player_spawn`, `player_death`, `player_jump`, `player_use`
- `use_primaryattack`, `use_secondaryattack`, `use_reload`
- `instructor_primaryattack` when a weapon is acquired
- `player_pushable_push`, `player_pushable_pull` when a pushable actually moves

The message carries one event entity index, classname, targetname, field name,
string value, integer value, and float value. This is a bounded single-player
event bridge, not the full Source `IGameEvent` registry or its game-specific
event schemas.

## Locator direction sprites

The HUD can use the original `icon_arrow_right.spr` and
`icon_arrow_up.spr` assets as the directional indicators around offscreen
targets. The opposite directions are rendered by mirroring the same sprite at
draw time: horizontal flip for left and vertical flip for down. No separately
authored left/down sprite files are required. If a directional sprite cannot be
loaded, that directional indicator is skipped; the main locator's existing
GoldSrc sprite/text fallback remains available.

## Keyboard binding keycap

The original `icon_key_generic.spr` is loaded as an optional keyboard keycap.
It is drawn behind short one- or two-character bindings in world-space locator
labels and managed lesson cards. Longer bindings continue to use the existing
drawn key-chip fallback because the original Source HUD uses a separate wide
key asset for those labels. If the generic sprite is missing, the drawn chip
remains available.

## Deliberate GoldSrc differences

- Multiple lesson opportunities, prerequisites, counters, and priority
  arbitration are retained, but the HUD can render only one managed lesson card
  at a time.
- Map-authored `env_instructor_hint` messages continue to own the existing
  hint/locator card directly and take precedence over scripted lessons. The
  PS2 interaction brackets and automatic pushable move/hold prompt remain
  separate systems.
- Source VGUI2 target anchoring, true Source locator target handles, the full
  Source hint/lesson event payload, split-screen/player swapping, team and
  input-device filters, and Source game-specific weapon/ammo/health actions
  are not implemented.
- `present start/end` do not run Source's VGUI animation lifecycle;
  `present complete` maps to the lesson having been displayed.
- Sprite names map to GoldSrc `.spr` files. Source `.vmt/.vtf` art is not
  converted or bundled.
- Counter cvars provide persistence; this is not Source's full saved-game
  lesson-state integration.

This separation keeps existing GoldSrc behavior intact while allowing map
authors and mod code to drive a scripted lesson manager with familiar Source
lesson definitions and rules.
