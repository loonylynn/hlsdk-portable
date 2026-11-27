# env_fog

`env_fog` is a map-wide fog controller for the Xash3D FWGS OpenGL renderer.

It combines a CS:CZ-style exponential density curve with Source-style start/end distance and radial camera-distance evaluation. The renderer evaluates fog per fragment in eye space, so radial mode follows camera-to-surface distance. Axial mode is available for a view-depth effect.

## Map properties

| Key | Default | Description |
|---|---:|---|
| `fogcolor` / `rendercolor` | `128 128 128` | RGB fog color |
| `startdist` / `fogstart` | `0` | Distance where fog starts |
| `enddist` / `fogend` | `3500` | Distance where fog reaches its configured maximum |
| `density` / `fogdensity` | `0.0003` | Shape of the exponential transition between start and end |
| `maxdensity` / `fogmaxdensity` | `1.0` | Maximum opacity, clamped to 0–1 |
| `radial` / `fog_radial` | `1` | 1 uses radial camera distance; 0 uses axial view depth |
| `skybox` / `fogskybox` | `0` | Apply fog to the skybox |

Spawnflag 1 starts the controller enabled. A map can activate or deactivate it through its targetname using `USE_ON`, `USE_OFF`, or `USE_TOGGLE`. Only one controller is active at a time; the last activated controller takes control.

## Hybrid fog curve

Let d be camera distance, s start, e end, rho density and m maximum density. The renderer clamps t=(d-s)/(e-s) to 0–1 and computes:

`fog = m * (1 - exp(-rho * t * (e-s))) / (1 - exp(-rho * (e-s)))`

When density is zero, the transition is linear. This preserves the familiar density control while reaching the configured maximum at the end distance.

The client renderer must have `gl_fog 1`. On the desktop OpenGL renderer, launch Xash3D with `-gl2shim` to enable the shader compatibility layer used for radial distance and the hybrid start/end/density curve. Without that option, classic fixed-function OpenGL falls back to its linear/exp fog behavior; the software renderer does not implement TriAPI fog.


## Console renderer test

The client DLL registers a local test override so the fog renderer can be evaluated without placing an `env_fog` entity in a map. This only changes the local client's render state; it does not change the server controller or test entity inputs.

```text
env_fog_test on
env_fog_test off
env_fog_test set <r> <g> <b> <start> <end> <density> [maxdensity] [radial] [skybox]
```

The default `on` profile uses a muted blue-gray radial fog. The `set` form accepts RGB values from 0–255, distances in world units, density from 0–0.1, maximum opacity from 0–1, and 0/1 switches for radial distance and skybox fog. For example:

```text
env_fog_test set 150 165 180 128 2200 0.0008 0.9 1 0
```

Use `env_fog_test off` to return to the fog state last received from the server. Testing still requires `gl_fog 1` and the desktop OpenGL `-gl2shim` launch option described above.
