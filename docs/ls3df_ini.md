# LS3DF.ini — optional graphics improvements

The port can do a few things the original engine cannot. They are all **off by default**: without
an `LS3DF.ini` the DLL behaves exactly like the original (the differential tests run this way). To
turn them on, put an `LS3DF.ini` in the same folder as `LS3DF.dll`:

```ini
[Graphics]
AnisotropicFiltering=16
TrilinearFiltering=1
ImprovedMipmaps=1
TrueColorTextures=1
TrueColorLightmaps=1
```

A missing key is off. The DLL reads the file once, at start-up.

## Options

| Key | Values | What it does |
|---|---|---|
| `AnisotropicFiltering` | 0, 2–16 | Anisotropic texture filtering, for sharper ground and walls at shallow angles. Capped at what the graphics card supports and reapplied after a device reset. |
| `TrilinearFiltering` | 0 / 1 | Mip levels are always blended, even if the game asks for point or no mip filtering. |
| `ImprovedMipmaps` | 0 / 1 | A new mip-map filter for BMP / TGA textures (details below). |
| `TrueColorTextures` | 0 / 1 | Textures loaded from image files are 32-bit (A8R8G8B8) instead of 16-bit or DXT. DDS files keep their own format, and textures the engine fills itself (shadow maps, video, lightmaps) are left alone. |
| `TrueColorLightmaps` | 0 / 1 | Lightmaps are always 32-bit. |

**ImprovedMipmaps** replaces the original's filter, which weighs each 2 × 2 block unevenly, rounds
down at every step and averages gamma-encoded values:

- Brightness is kept. The original's mips of random-noise textures lose 24% of their light at the
  first level and 47% at 1 × 1; the new filter stays within half an 8-bit step.
- Colours are averaged in linear light (gamma-correct) and weighted by alpha, so colour-key
  edges no longer pick up the transparent colour (black fringes around foliage).
- Colour-keyed (alpha-tested) textures keep their coverage at each level. With the original's filter
  and the engine's alpha reference (0x20), foliage fattens with distance: 30% leaf coverage became
  100%.
- These textures get their own cache files (a `G` before the extension), so old cache files are
  never used for them.

## Things to know

- **Trilinear filtering and 32-bit lightmaps are already the engine's defaults.** Those two
  options only matter if the game turns them down. If it does, a line in `log.txt` says so.
- **`log.txt` lists what is in effect** under "Port enhancements (LS3DF.ini)", including the
  anisotropy the card actually allows. These lines are written even when the game's log is off.
- **`TrueColorTextures` uses much more memory**: about 8× the texture memory of the default
  compressed setting (all of the game's textures: 1.9 GB at 32-bit against 233 MB as DXT1, of which
  a mission loads a part). The game is a 32-bit process, so if a large map runs out of memory, turn
  this option off first.
- Very fine alpha detail, such as grass blades one pixel wide and a few pixels apart, still
  disappears at a distance; no per-level filter can keep it. It no longer turns into a solid wall.
- **Not tested in the game yet.** The texture and loading paths are verified (below); the filter
  settings on a real Direct3D device are not.

## The changes

- New: [`src/core/enhance.h`](../src/core/enhance.h) / `enhance.cpp` (reads the INI; `Enhancements()`,
  `SetEnhancements()` for tests, `EnhancementLog()`), and
  [`src/igraph/mip_filter.h`](../src/igraph/mip_filter.h) / `mip_filter.cpp` (`DownsampleMip`,
  `ScaleAlphaToCoverage`, `AlphaCoverage`).
- `igraph/itexture.cpp`: the 32-bit override, the new mip chain (each level made from the unscaled
  level above; only the uploaded copy of a colour-keyed level has its alpha rescaled), the `G` cache
  file tag.
- `igraph/igraph.cpp`, `igraph_state.h`: the anisotropy limit from the device caps (`g_anisotropy`,
  `LinearMinFilter()`), the log block.
- `i3d/driver_core.cpp`: `SetState(0)` uses the anisotropic minification filter and sets the
  maximum anisotropy on stages 0–2, `SetState(2)` the trilinear floor, `LightmapsTrueColor()`
  (also used by `lightmap.cpp` and `litobject.cpp`); `driver_draw.cpp` restores the filter after
  drawing text.
- Every change is marked `(Port enhancement, LS3DF.ini <key>)` in the source.

## Tests

- `test_enhance`: the INI reader and the filter's properties (uniform colours unchanged, the
  gamma-correct average, no colour from transparent texels, brightness across a chain, coverage on
  several shapes, every size down to 1 × 1 without writing past the level), with the original's
  filter printed alongside.
- `compare_texture [original LS3DF.dll] [rounds] enhanced | mipmaps`: the original is asked for
  `ITC_TRUECOLOR` where the option forces it; level 0 must match, and every further level must be
  exactly the new filter's chain (16-bit levels too, through `WriteSurface`); also the port's own
  cache files and their names. 8,000 cases each, and three deliberately introduced bugs are caught.
  Without a mode, it still matches the original on all 8,000 cases.
- `compare_loader` without an INI: all 6,214 game files identical. With every option on (INI next
  to `%TEMP%\ls3df_loader\LS3DF_port.dll`), run it in chunks
  (`compare_loader.exe "" "" <first> <count> "" all`): in one process the test's fake devices keep
  every texture, and at 32 bits they fill the 4 GB address space. All files load; the differences
  are texture formats and levels, plus texture ids one lower from `d_lamp_05` on. That is because
  `2DE_RAC1.TGA` is asked for with and without `ITC_TRUECOLOR`, and with the option both requests
  are identical, so the port shares one texture.
