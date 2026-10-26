# LS3DF.dll — source port

Clean C++17 rewrite of `LS3DF.dll`, the LS3D engine (version 5.59.5, build of 16 August 2004)
that runs Hidden & Dangerous 2 and Sabre Squadron:

- **I3D** — the scene graph (frames, visuals, sectors and portals, lights, sounds, models and
  animation), the renderer, collision queries, lightmaps, the model / scene / animation loaders
  and the vertex / index buffer managers;
- **IGraph** — the window, the Direct3D 8 device, textures and image loading (BMP / TGA / DDS,
  DXT compression), screenshots, DirectInput;
- **IShow** — video into textures (DirectShow, Ogg Vorbis, Bink);
- **ISND** — DirectSound sounds, 3D sounds and the listener, EAX, the voice manager.

It builds into a drop-in replacement DLL: the same 60 exports with the same ordinals
([ls3df.def](ls3df.def)), and the same class layouts and virtual tables, because the game reads
and calls engine objects directly. Every function carries the original's address as a
`// 0x1000xxxx` comment. The original's quirks that the game can observe are kept (and
commented); plain memory bugs (reads of freed memory, overruns, leaks, endless loops) are fixed,
each with a comment. [docs/porting_notes.md](docs/porting_notes.md) lists them by module.

The companion port of `rw_data.dll`, the game's data-file layer, is at
<https://github.com/RellHaiser/rw_data>.

## Status

> **Tried briefly in the game.** In a first session (October 2026) the built DLL ran Hidden &
> Dangerous 2 without obvious problems: the menus, two cooperative multiplayer missions, the
> music, and the intro and credits movies. Much of the DLL is also verified against the original
> by the differential tests below. A first session is not a play-through: the single-player
> campaign, Sabre Squadron and the less common paths (mode changes, device loss and reset) are not
> known to work yet. Use it in a copy of the game folder and keep the original DLL.

| Part | Verified against the original by |
|---|---|
| Math exports (vectors, matrices, quaternions, planes) | `compare_math`: all 55 bit-exact under the game's FPU precision |
| DXGetErrorString8 | `compare_dxerr8`: 2,067,333 result codes |
| Image resizing | `compare_resize`: 30,000 images |
| Textures: image loaders, conversion, colour keys, mip maps, cache files, DDS / DXT decoding | `compare_texture`: 6,000 cases |
| The DXT (S3TC) encoder | `compare_s3tc`: 23,000 images, 373,000 blocks |
| Sound driver, sounds, 3D sounds, listener, voice manager | `compare_isnd`: 1,000,000 operations on fake DirectSound objects |
| IGraph's keyboard, mouse and timer | `compare_input`: 400,000 operations on fake DirectInput objects |
| IGraph's window procedure and message loop | `compare_window`: 400,000 operations on hidden test windows |
| Frames, lights, cameras, sectors, portals, occluders, areas, models, animation | `compare_frame`: 800,000 random operations |
| Materials | `compare_material`: 800,000 operations |
| Vertex / index buffer managers | `compare_buffers`: 400,000 operations, node for node |
| Meshes, mesh levels, face groups | `compare_mesh`: 500,000 operations |
| Collision queries (rays, picking, moving spheres) | `compare_collision`: 2,000,000 random queries and scene changes |
| The lightmap baker | `compare_lightmap`: 20,000 rounds of bakes and light probes |
| Model, scene, animation and shadow loading | `compare_loader`: all 6,200 of the game's files plus 14 synthetic ones, member by member |
| IShow: Ogg music streaming, Bink movies, AVI and other files through DirectShow, into textures | `compare_show`: 80,000 operations on the game's music and movies, 24 AVI playbacks |
| The scene's weather: rain and snow particles, their vertices, the weather settings | `compare_weather`: 140,000 frames, every vertex |
| The rest of the renderer, IGraph's device and window creation, ISND start-up, IShow's full-screen playback and drawing | not compared with the original; they ran in the first game session (above) |

The tests are described [below](#tests); each one's header says what it compares and what it
leaves out.

## Using the DLL

`python build.py` makes `build\msvc\LS3DF.dll` (32-bit, the C runtime linked statically: nothing
to install). In a **copy** of the game folder, replace `LS3DF.dll` with it. It needs the game's own
`rw_data.dll`, `binkw32.dll` and `vorbisfile.dll` / `ogg.dll` (shipped with the game) and the
system's `d3d8.dll`, DirectInput 8 and DirectSound.

## Building

Requirements:

- Windows with Visual Studio 2019 or 2022 and its C++ x86 tools ("Desktop development with C++");
  `build.py` finds them with `vswhere`;
- Python 3;
- MinGW's `dlltool` (32-bit; MSYS2's mingw32 package, for instance), used once to make the import
  libraries of `binkw32`, `rw_data` and `vorbisfile` from [lib/](lib) — set `DLLTOOL` to its
  path if it is not on `PATH`.

```bat
python build.py          rem the DLL: build\msvc\LS3DF.dll (+ .pdb)
python build.py check    rem compile every file, no link (and check the device boundaries)
python build.py test     rem the tests: build\compare_*.exe
python build.py --debug  rem a debug build
```

Intermediate files go to `%TEMP%\ls3df_build` (a synced source folder such as Google Drive or
OneDrive locks freshly written files and breaks the compiler).

## Tests

The tests in [tests/](tests) are differential: each loads the **original** `LS3DF.dll` from your
copy of the game (it is not part of this repository) and compares it with the port, most of them
on random operations with a seed, some on the game's own files. They run the original on fake
Direct3D / DirectSound objects, so they need no graphics or sound hardware. (`compare_show` also
plays test files through Windows' own DirectShow, and the sound card when there is one: without
one, the files with sound fail to open on both sides alike.)

- The original: the test's first argument, else `%HD2_DIR%\LS3DF.dll`, else the `LS3DF.dll` on
  the DLL search path. The game folder must be on `PATH` in any case (the original imports
  `rw_data.dll`, `binkw32.dll`, `vorbisfile.dll` and `ogg.dll` from it). A DLL that is not the
  original MSVC 6 build is refused.
- `compare_loader` loads the port's built DLL (`build\msvc\LS3DF.dll` with its `.pdb`) next to
  the original, mounts the game's `.dta` archives, and takes the list of file names from the
  archives unpacked to folders: `%HD2_UNPACKED%`, else `HD2_Unpacked` next to the game folder.
  `compare_weather` loads the built DLL the same way (and needs no game files).
- `compare_show` plays the game's music from the unpacked `Sounds` folder (found the same way) and
  its movies from the game folder's `Gamemovie`.

```bat
set PATH=C:\Games\Hidden and Dangerous 2;%PATH%
cd build
compare_math.exe "" 50000 --pc24
compare_frame.exe "" 200000 1
compare_loader.exe "" "" 0 100000
```

Each test prints what it covered and ends with "identical" / "0 differ" (exit code 0) or the
first difference (exit code 1); `compare_loader ... "" all` lists every difference instead.

## Layout

| Path | What |
|---|---|
| `src/core` | log and errors, version, CPU detection, engine start-up (`DllMain`), containers (the original's dynamic array, MSVC 6's multiset) |
| `src/math` | the math exports |
| `src/i3d` | the scene graph, renderer, collision, lightmaps, loaders, buffer managers |
| `src/igraph` | IGraph, textures, image loading and conversion, the DXT encoder, IShow |
| `src/gfx` | the render device all drawing goes through, and its Direct3D 8 backend ([docs/render_device.md](docs/render_device.md)) |
| `src/audio` | the sound device all sound goes through, and its DirectSound backend ([docs/sound_device.md](docs/sound_device.md)) |
| `src/input` | the input device IGraph reads the keyboard and mouse through, and its DirectInput backend ([docs/input_device.md](docs/input_device.md)) |
| `src/platform` | the window and its messages, the cursor, time, threads, locks and events, and their Win32 implementation ([docs/platform.md](docs/platform.md)) |
| `src/video` | the video device IShow plays its movies and DirectShow files through, and its Windows backend: Bink, DirectShow and the texture renderer ([docs/video_device.md](docs/video_device.md)) |
| `src/isnd` | ISND |
| `src/dshow` | the DirectShow base classes the video backend's texture renderer is built on |
| `src/dx` | Direct3D 8 declarations (the Windows SDK no longer has them): the types the renderer uses (`d3d8_types.h`), the interfaces only the backend uses (`d3d8.h`); DirectSound's and DirectInput's types (`dsound_types.h`, `dinput_types.h`); DXGetErrorString8 |
| `src/external` | the interfaces of `rw_data.dll`, Bink and vorbisfile |
| `tests` | the differential tests and their fake Direct3D device |
| `lib` | import definitions for the DLLs the engine imports |
| `ls3df.def` | the exports and their ordinals |

## Third-party code

The original statically links code it did not write; the port rewrites it from the binary as
well:

- the DirectShow base classes (`strmbase.lib` of the DirectX 9 SDK, © Microsoft — Microsoft now
  publishes their source under the MIT licence in Windows-classic-samples);
- `DXGetErrorString8` (`dxerr8.lib` of the DirectX 8.1 SDK, © Microsoft): its table of result
  code names;
- the S3TC (DXT) encoder library;
- the tree behind MSVC 6's `std::multiset` (Dinkumware), kept because the buffer managers depend
  on its exact shape.

`src/dx/d3d8_types.h` and `d3d8.h` declare Direct3D 8 as Microsoft's SDK did; `src/external`
declares the interfaces of Bink (RAD Game Tools), vorbisfile (Xiph.Org) and `rw_data.dll`.

## Disclaimer

This is an unofficial fan project, not affiliated with or endorsed by Illusion Softworks,
2K / Take-Two Interactive or any other holder of rights to Hidden & Dangerous 2. The source was
written by analysing the behaviour of the original DLL; the repository contains no files of the
game (no binaries, data or assets). You need your own copy of the game to use or test it.
Trademarks belong to their owners.
