# Title screen

Dawn gives the Destiny 2 boot flow its own look. Dawn's own artwork for the
title screen is swapped in before the game loads it, the Bungie splash and
character select are recoloured from the game's own texture bytes as they
load, and two renderer layers draw over the finished frame: one inverts the
light-grey loading screens the texture route cannot reach, the other draws the
moving filigree behind the title. Nothing the game draws is moved or re-timed,
and nothing derived from Bungie's art is embedded in the DLL.

| Setting (`client` in `Dawn/settings.json`) | Default | Off means |
| --- | --- | --- |
| `custom_bootflow_textures` | true | The stock title, splash and character-select art. |
| `invert_splash_screens` | true | The loading screens stay light grey. |
| `title_filigree` | true | No medallions and no halo round the wordmark. |
| `dump_gpu_entries` | false | Research aid. On, every GPU entry the dispatcher receives is logged (class, tag, size, first bytes) and every texture payload is written once to `Dawn/exports/texdump/`, up to about 1 GB per launch. |

## What is on screen

- **Loading screen** (from about 5 s after launch until the splash, and again
  for about 7 s behind Enter on the title): the game's light-grey pattern,
  tricorn and cycling emblem, inverted at runtime to a near-black navy with the
  marks a faint blue-grey. Dawn's busy overlay sits on it with its progress bar
  in the logo blue.
- **Bungie splash** (about 28..34 s): dark, from the game's own textures. The
  marble, the cloud haze, the turning square and the BUNGiE wordmark are
  recoloured in memory as the game loads them; the dot on the i keeps its
  stock sky blue at 55 % brightness. The dissolve into the title is the game's
  own.
- **Title**: a dark navy base with a flat lighter-blue field, the Dawn sunrise
  icon with DESTINY 2 across its upper half, the DAWN wordmark (Neue Haas
  Grotesk Display Bold, the game's own headline face) between the icon's
  horizon and its reflection lines, breathing with a soft blue halo every 4 s,
  a very faint static V of Julia medallions in the bottom corners, and two
  mirrored Julia medallions filling the top three quarters of the frame,
  morphing in place. The logo's blue is a hitbox to them: one outline 2 px off
  the blue, the fractal's lines swept along it and stopped there, bright nodes
  where they meet.
- **Waiting screen and character select**: the marble, square and line art the
  splash used, recoloured the same way under the post-title package's tags; the
  class cards, text and chat box are untouched. The small loading emblem at the
  bottom right takes the boot screen's blue at runtime.

Verified in game on 2026-09-21 at 16:9 (the 1936x1119 borderless window) and
at 21:9 (a 1920x823 window), through boot, Enter on the title, character select
and the exit to orbit.

## Texture override

[bootflow_texture_override.cpp](../Dawn/src/client/hooks/bootflow/bootflow_texture_override.cpp)
ports Sunrise 0.4.0's override. It detours Resourcerer's GPU-entry dispatcher
(found by signature in the main image) and, for texture entries whose TagHash
is in its table, hands the game other pixels than the package's. Installation
runs from [steam_lifecycle.cpp](../Dawn/src/steam/runtime/steam_lifecycle.cpp)
right after the package-trust bypass, because the bootflow package loads
before the first Steam callback pump; removal runs from
[client_runtime_lifecycle.cpp](../Dawn/src/client/runtime/client_runtime_lifecycle.cpp)
before the bypass is removed. The stock package stays registered and unchanged.

- 37 `AssetSpec` rows. The 23 *replace* rows hand over one of the 23 DDS files
  embedded in `Dawn/resources/bootflow/` (`IDR_BOOTFLOW_TEXTURE_*` 2007-2039 in
  [resource.h](../Dawn/resources/resource.h) and [dawn.rc](../Dawn/resources/dawn.rc)),
  all of them Dawn's own art or fully transparent placeholders. The 14
  *transform* rows recolour the game's own bytes in memory
  ([bootflow_texture_recolour.cpp](../Dawn/src/client/hooks/bootflow/bootflow_texture_recolour.cpp))
  and hand the result over, so no Bungie-derived pixels exist anywhere in the
  build. Every payload arrives under the DESCRIPTOR tag; a row's header tag is
  0 when the payload tag is not known (the post-title package's copies), and
  such a row matches on the descriptor tag alone.
- The transform rule is the generator's: every texel becomes (1 - c) x
  (0.55, 0.72, 1.0) x 0.45 with its alpha kept, in the same float32 arithmetic,
  and on the wordmark the tittle, found by its blue minus red, keeps its stock
  hue at 55 %. The two marbles are BC7: each block is decoded by an all-mode
  decoder ([bc7_codec.cpp](../Dawn/src/client/hooks/bootflow/bc7_codec.cpp)),
  recoloured, and encoded again in mode 6 by a port of the generator's encoder.
  The result is byte-identical to the DDS files the generator used to produce
  (checked on 2026-09-21 against the game's dumped bytes: every RGBA8 byte and
  all 288,000 marble blocks). Each distinct payload is transformed once, about
  50 ms per marble on up to half the machine's cores, and kept for the
  process like a resource; the post-title copies are byte-identical and find it
  by hash. A payload of an unexpected size goes through untouched
  (`stage=transform ... result=skip`).
- The descriptor replacement never fires on build 86657 (the 0xCAFE marker
  check fails), so an embedded file must keep the stock dimensions and format:
  an RGBA8 legacy DDS (straight alpha, no mips). A wrong size renders garbage.
- Every listed resource must load, or the whole override is skipped
  (`stage=resources result=fail`).

| Tags | Stock size | Content |
| --- | --- | --- |
| `80A145FF`, `80A14607` | 1920x1200 | The title background pair, drawn 1:1 with rows 60..1139 visible; only the right half of each texture shows, filling the left half of the screen and mirrored for the right. The flat navy base; the blue field, the corner V and the sunrise icon. |
| `80A1461F 21 24 25 28 29 2B 2E 2F 31 33 36` | 1110x61 | The 12 language copies of the wordmark strip, drawn at screen y 542..602: DAWN. |
| `80A1460E 10 13 17 19 1D` | 354, 348, 328, 276, 250, 136 | The counter-rotating, 2x2-mirrored ring slots round the logo: transparent (the ring of effects was rejected as clutter). |
| `80A14604`, `80A146D3` | 800, 824 | The two large spinning blocks: transparent. The game spins their copies about fixed, unrelated centres, which can neither mirror nor morph, so the medallions are drawn by the DLL. |
| `80A14601` | 345x490 | The static panel behind the logo: transparent. |
| `80A146CC`, `80A146CF` | 1920x1200 BC7 | The splash marble (two variants): recoloured from the game's bytes at boot, decoded, darkened and re-encoded. |
| `80A14639` | 1920x1080 | The cloud haze over the marble: recoloured at boot, the same alpha, black. |
| `80A146D5` | 912 | The splash's slowly turning square: recoloured at boot, dark lines. |
| `80A146A9`, `80A125B7` | 495x135 | The BUNGiE wordmark (the bootflow and startup-package copies, both dispatched, the payload padded to 267,392 bytes): recoloured at boot, the letters a faint blue-grey, the tittle its stock blue at 55 %. |
| `80B46A3B 3E 41 44`, `80B47F54`, `80B4615A`, `80B47F58`, `80B4615D` | as above, 824 | The post-title package's byte-identical copies of the marbles, the square and the 824 line art (the waiting screen and character select): recoloured the same way when they load, the copies reusing the first result. |

The loading screen is drawn from nothing the dispatcher sees (nothing at all is
dispatched before about 19 s after launch; the whole bootflow package then lands
in one batch), which is why it is inverted at runtime instead.

## Boot-screen inversion

[graphics_splash_invert.cpp](../Dawn/src/client/hooks/graphics/renderer/graphics_splash_invert.cpp)
copies the finished frame each boot frame and redraws it through a pixel shader
compiled at runtime (ps_4_0, `D3DCompile`) on one full-screen quad in Dear
ImGui's background draw list, so Dawn's own overlays keep their colours on top.
The presentation hooks attach from `steam::initialize`, so the game's first
presented frame is already covered. A 16x9 grid of texels is read back every
frame (channel mean, darkest, blob, bright and saturated shares) and steers the
pass:

- Strength follows the frame mean (a smoothstep over 0.25..0.45), so only
  bright frames are touched; the dark splash, the title and character select
  pass through.
- Pixels above a luma line are inverted, scaled by 0.45 and tinted
  (0.55, 0.72, 1.0), the rule the splash textures are recoloured with, so the
  two match. Pixels below it are washed towards the Dawn blue.
- `Entering state 'bootflow:start'` (the title), or the dissolve showing
  through, ends the boot phase. The layer then stays ARMED (`stage=menus`)
  through the `bootflow:` sign-in states and `character:` select, inverting the
  loading screen behind Enter; the crossfade frames round it are dimmed to that
  screen's darkness, only above a mean floor that spares the title.
- The emblem rule: on a dark frame whose sample grid holds no saturated texel,
  neutral dark pixels in the bottom-right window are shifted onto the boot
  tint's hue at their own brightness (the waiting screen's grey loading emblem).
  This gate held at 16:9 and 21:9; a channel-mean window did not.
- The first state outside `bootflow:` and `character:` ends it
  (`reason=state`), or 30 min. `acting()` and `covering()` report what the
  layer did to the frame just drawn, for the filigree.

Every constant is a `kXxx` at the top of the file, documented in place.

## Title filigree

[graphics_title_filigree.cpp](../Dawn/src/client/hooks/graphics/renderer/graphics_title_filigree.cpp)
draws the medallions the same way (a runtime shader on a full-screen quad, fed
by the frame copy and two field textures):

- Two Julia medallions, the right one the exact mirror of the left: the
  reflected point is evaluated, with a soft fold at the centre line. The Julia
  constant walks a small ellipse round -0.765 + 0.18i every 20 s, checked to
  stay outside the Mandelbrot set, so the curls change shape in place; the
  contour lines also crawl inwards slowly.
- Escape-time contours (budget 70) drawn as ~2.6 px lines in three colours
  cycled by band; lines packed tighter than ~2 px fade out instead of aliasing;
  a radial fade towards the medallion's edge.
- Keyed under everything bright on the frame (the logo, the text, the prompts).
- The wall: on the first title frame the layer masks the brand-blue texels of
  the back buffer and builds a blurred pull field (quarter resolution) and an
  exact Euclidean distance transform of the mask (full resolution, R8). The
  shader sweeps the sampling point along the outline's tangent near the blue,
  stops the lines short of it by a 2 px gap plus the line width, draws one
  outline line there and brightens both where they meet. Nothing crosses the
  logo. The field is built once; the logo does not move.
- The wordmark pulse: light, low-saturation glyphs inside a window round the
  wordmark strip are blurred into a Gaussian halo (sigma 8 px) added in
  (110, 170, 255), and the glyphs are lifted 10 %, times a raised cosine with a
  4 s period.
- Lifetime: starts after `bootflow:start` once the inversion has let go, but
  draws nothing until the wall has found the logo's blue (the title's first
  frame), then fades in over 1.5 s. It stays out of any frame the inversion
  acted on, hides on a bright frame (the loading screen behind Enter) and ends
  once that has lasted a second, or on the first state outside `bootflow:`.
- Cost: two 70-iteration escape loops per pixel plus the halo taps every frame
  while the title is up, a few ms of GPU at 1080p; about 26 MB transiently for
  the distance transform.

Both layers take every world-controller state by name from
[retail_log_enqueue_observer.cpp](../Dawn/src/client/hooks/retail_log/retail_log_enqueue_observer.cpp),
which parses `Entering state '...'` before its own log-level check, and are
released with the device in
[graphics_renderer_lifecycle.cpp](../Dawn/src/client/hooks/graphics/renderer/graphics_renderer_lifecycle.cpp).
The [busy overlay](../Dawn/src/core/ui/busy/ui_busy_overlay.cpp) pushes the
logo blue (#0859F2) round its progress bar only; the theme's histogram colour
still marks unavailable activities on the mission cards.

## Textures

The 23 DDS files are generated outside this repository by
`build_title_textures.py` in the "Dawn assets/title-screen" package (its `P`
dict holds every knob: background, icon, wordmark, corner V, and the splash
rule's gain, tint and tittle settings, which the recolour code mirrors;
`preview_dll_filigree.py` is a port of the filigree shader, for judging a look
before a build). `Dawn/resources/bootflow/` must equal the package's
`bootflow/` folder. The generator also writes the seven recoloured splash and
character-select textures, but only into a local `reference_splash/` folder:
they are the reference the runtime recolour is checked against and are never
packaged. A new embedded slot needs its stock size and format (the descriptor
tag's payload entry, from a `dump_gpu_entries` run or the research inventory at
https://github.com/di-donna/dawn-title-screen), a resource id, an `.rc` line and
an `AssetSpec` row; a new transformed slot needs its payload size and a rule.

## Build and validation

From the repository root, with the MSBuild 18 tool set the projects require:

```powershell
python tools/title_screen/compile_check.py
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild Dawn/Dawn.vcxproj -p:Configuration=Release -p:Platform=x64 -p:PreferredToolArchitecture=x64 -m:4 -v:minimal -nologo
& $msbuild Dawn/unit/bootflow_recolour_tests.vcxproj -p:Configuration=Release -p:Platform=x64 -p:PreferredToolArchitecture=x64 -v:minimal -nologo
& ./build/unit/bootflow_recolour/Release/bootflow_recolour_tests.exe
```

[compile_check.py](../tools/title_screen/compile_check.py) extracts both
runtime shader templates, fills them as the `.cpp` files do and compiles them
with `d3dcompiler_47`, then checks the filled size against each layer's
`kShaderSourceCapacity`. A shader that fails at runtime only shows as
`stage=shader result=fail` and a layer that never draws (a template one byte
over its buffer once shipped a build without any filigree). Keep the script's
argument order in step with the `snprintf` calls when a template gains a slot.
`line` and `half` are reserved words in HLSL.

The [recolour suite](../Dawn/unit/bootflow_recolour_tests.cpp) runs 20 checks:
the rule on texels worked out by hand (white to black, the tint at the gain,
the kept tittle blue), the BC7 codec's round trip and its mode bits, and the
transform's size checks. With arguments the same executable is a tool,
`decode <blocks> <rgba>` and `transform <darken|wordmark|bc7> <in> <out>`, for
checking the code against the game's own bytes: dump one launch with
`dump_gpu_entries`, transform each payload, and compare with the generator's
`reference_splash/` files (byte-identical on 2026-09-21) and the decoder with
the `texture2ddecoder` package (identical on random blocks of every mode).

Replace `steam_api64.dll` (the root and `bin/x64`) with the game closed, and do
not launch within about 8 s of writing it. With client logging at info, look in
`Dawn/logs/dawn.log` for:

- `ev=bootflow_texture stage=attach count=37 result=ok`; `stage=setting enabled=0 result=skip` when off; `stage=resources result=fail` when an embedded file is missing; `stage=transform tag=... rule=... size=... ms=... result=ok` once per recoloured payload (`result=skip` with `expected=` when a payload is not the size the rule was written for).
- `ev=splash_invert stage=shader result=ok`, `stage=start`, `stage=closing reason=state|dissolve`, `stage=menus`, `stage=end reason=state|menus_timeout|title|timeout|blend_state`.
- `ev=title_filigree stage=shader result=ok`, `stage=start`, `stage=wall ... result=ok|retry`, `stage=hide|show mean=`, `stage=end reason=bright|state|shader|blend_state`.

Neither check replaces an in-game look at the boot, the title, Enter on the
title and character select; every per-frame decision above uses the sample
grid's CHANNEL mean, which sits within a few hundredths of the title's and
character select's values, so judge a gate change from captures at both aspect
ratios.

## Known limits

- The loading screen can only be darkened at runtime; nothing it is drawn from
  passes the dispatcher (the startup package's fonts are the suspect).
- The recolour runs on the dispatcher's thread when the bootflow package loads,
  about 20 s after launch, before any of these textures is on screen; a
  different game build with other payload sizes would pass its textures
  through untouched rather than recolour them.
- The inversion samples the back buffer synchronously and copies it once per
  frame during boot; harmless at 1080p, 64 MB per frame at 4K HDR.
- The wall field is built from the first title frame with enough blue; an
  animated logo would leave the outline stale.
- The loadout catalog's progress bar is still Dear ImGui's yellow; only the busy
  overlay's bar was recoloured.
