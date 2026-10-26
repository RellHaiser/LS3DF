# Porting notes

## The original

`LS3DF.dll` is LS3D Engine V5.59.5, built on 16 August 2004 with MSVC 6 (release, image base
0x10000000, no RTTI, no debug information). Its strings name the source tree
(`C:\MyProjects\Engine8_5\I3D\*.cpp` — `IBManager.cpp`, `VBManager.cpp`, `load.cpp`, … —
and `C:\MyProjects\Engine8_5\IGraph\IGraph.cpp`).

- `.text` 0x10001000–0x1009afff: the engine, with the import stubs of DirectX / vorbisfile linked
  in between, then the statically linked MSVC 6 C runtime from 0x1008d41c.
- `.rdata` 0x1009b000: virtual tables 0x1009b400–0x1009df00 and 0x100a1fa8–0x100a2478, the
  DXGetErrorString8 tables, the import and export directories.
- `.data` 0x100a6000: the static initializer list (172 entries) and the engine's globals.
- 60 exports: the math classes (S_vector, S_matrix, S_quat, S_plane, Rotate*Matrix, RandFloat),
  I3D_frame::UpdateWMatrixProc, I3D_visual::UpdateWBoundProc, GetEngineVersion, TEV, GetIGraph,
  I3DGetDriver, I3DGetResultMessage, ISndGetDriver, dbgPrintf.
- Imports: d3d8, DINPUT8, DSOUND (by ordinal), WINMM, vorbisfile, rw_data, binkw32, VERSION,
  ole32 (DirectShow), USER32, GDI32, KERNEL32.

The frame factory (0x10018e40) and the visual factory (0x10019750) keep a free list per type
(0x100aded8, 0x100adb10); the visual types are indexed 0–11 in
"OBJ_LMAPSNGMSMMRBBRDMRPHLENSPROJMIRREMTRLNDPSHD".

### Module map

Address ranges are the original's object files; virtual tables in brackets.

| Range | Contents |
|---|---|
| 0x10001000–0x10004c60 | projected geometry (frustum set-up, clipping for projectors and shadows) |
| 0x10004c60–0x100054b0 | I3D_animation_set [0x1009b400] |
| 0x10005550–0x10005f40 | I3D_area [0x1009b708] |
| 0x10005f80–0x100078e0 | billboard visual BBRD [0x1009c440] |
| 0x10007dd0–0x10009610 | lens flare visual LENS [0x1009c2c0] |
| 0x10009840–0x1000c680 | I3D_camera [0x1009bd70] |
| 0x1000c7a0–0x10015050 | scene collision and picking (I3D_scene slots 0xFC–0x114) |
| 0x10015100–0x1001b8f0 | I3D_driver [0x1009b640]: init, the factories, the frame name tree |
| 0x1001b8f0–0x1001ff20 | I3D_frame |
| 0x1001ff20–0x10020a20 | the animation stack and tracks [0x1009cac0] |
| 0x10020a70–0x10020c80 | I3D_joint [0x1009b9d8] |
| 0x10020c80–0x100232b0 | I3D_light [0x1009be08] |
| 0x100232d0–0x1002a530 | lit object visual LMAP [0x1009c6d8], the lightmap baker (0x100237c0) |
| 0x1002a530–0x1002c400 | I3D_material, I3D_anim_material (from 0x1002b030) |
| 0x1002c450–0x10030990 | the math exports |
| 0x10030990–0x10032230 | I3D_face_group, I3D_mesh_level, I3D_mesh |
| 0x10032280–0x10033fb0 | mirror visual MIRR [0x1009c138] |
| 0x10033fe0–0x10034f10 | I3D_model [0x1009ba50] |
| 0x10034f40–0x10035f70 | morph visual MRPH [0x1009c378] |
| 0x10035fc0–0x10038410 | object visual OBJ_ [0x1009c7c8] |
| 0x10038490–0x1003aa70 | I3D_occluder [0x1009b8e8] |
| 0x1003aa80–0x1003e790 | particle emitter EMTR [0x1009c050] |
| 0x1003ef70–0x10043210 | sector and portal building |
| 0x10043210–0x100436e0 | I3D_pose, I3D_pose_set |
| 0x10043730–0x10046200 | projector visual PROJ [0x1009c1f0] |
| 0x100462b0–0x1004c730 | I3D_scene [0x1009b798], weather |
| 0x1004c9b0–0x1004e510 | I3D_sector [0x1009bc20] |
| 0x1004e5d0–0x10050770 | I3D_shadow |
| 0x100507e0–0x10051d20 | shadow visual SHD |
| 0x10051dc0–0x10053b90 | skinned mesh visual SNGM [0x1009c5f8] |
| 0x10053c00–0x100542c0 | skinned morph visual SMMR [0x1009c510] |
| 0x10054310–0x10056150 | I3D_sound [0x1009bcb8] |
| 0x10056250–0x10056e80 | I3D_target |
| 0x10056ed0–0x10056f30 | I3D_user [0x1009bb30] |
| 0x10056f80–0x10057660 | I3D_visual |
| 0x100576b0–0x10057960 | I3D_volume [0x1009b960] |
| 0x10057960–0x10058ee0 | C_IBManager (IBManager.cpp) |
| 0x10058ee0–0x1005d5b0 | lightmap atlas creation |
| 0x1005d5b0–0x1005edc0 | the loader (load.cpp): models, scenes, animation sets, shadows, meshes |
| 0x1005edc0–0x10066010 | I3D_driver::Render and its helpers |
| 0x10066020–0x10068620 | C_VBManager (VBManager.cpp) |
| 0x10068620–0x10069a00 | CPU detection, version banner, the small exports, error reports |
| 0x100699a0–0x10070eb0 | IGraph [0x1009d840] |
| 0x10070fc0–0x10073720 | IShow [0x1009d9c0], the texture renderer filter |
| 0x10073760–0x1007a100 | ITexture [0x1009dc34], image loaders, texture streaming |
| 0x1007a100–0x1007d3e0 | ISND: listener, sound, driver, stream, source |
| 0x1007d466–0x1007fe12 | DXGetErrorString8 (dxerr8.lib) |
| 0x1007fe18–0x10086500 | the S3TC encoder library |
| 0x10086530–0x1008d41c | the DirectShow base classes (strmbase.lib) |

## Conventions

- Every function has its original address in a `// 0x1000xxxx` comment.
- Interfaces are `virtual __stdcall` (COM style: `this` on the stack), internal virtual functions
  `__thiscall`; class layouts and vtable slots are the original's (static_asserts in the headers),
  since the game reads members and calls slots directly.
- Behaviour the game can observe is kept, quirks included, with a comment. Plain memory bugs are
  fixed with a comment that says what the original did.
- Floating point: the game runs under Direct3D's single-precision FPU mode (PC_24), so the port
  computes in `float` in the original's operation order (sums are not reassociated; the inlined
  vector normalizations sum the squares in four different orders, `SquareXYZ` … in
  `src/math/ls3d_math.h`). Where x87 keeps a FSIN / FCOS / FPATAN result on the stack the port uses
  the extended value once; MSVC 6's `acos` keeps the caller's precision. Results can still differ
  where intermediates leave float's range (x87 keeps a 15-bit exponent).
- Visual types are fourCCs in memory order: `FourCC("BBRD")` (0x44524242), not the
  multi-character literal `'BBRD'` (0x42425244).
- Direct3DCreate8 is bound at run time (current Windows SDKs ship no `d3d8.lib`).

## Notes by module

### Math

All 55 exports are bit-exact against the original under PC_24. MSVC 6's `acos` is emulated as
`atan2(sqrtf((1 + x) * (1 - x)), x)`.

### IGraph and textures

- DrawPrimitive's transformed vertex formats are 0x1C4 / 0x2C4 (with D3DFVF_SPECULAR, as IShow's
  0x20-byte vertices need), not 0x144 / 0x244.
- Image resizing (a box filter) keeps the x87 operation order.
- ITexture's constructor in the original leaves the size, format, colour key, level count and
  stream record unset until Create (the stream record only for streamed textures); the port starts
  them at 0.
- Texture cache files (`*_N.565`, `.DX1` …) are written next to the image's path, as in the
  original.
- The held key of `IGKEY_REPEAT` starts as key 0, as the original's zero-initialised variable does:
  the first `GetKey` in repeat mode returns 0. (The port started it at -1 until `compare_input`
  found the difference.)

### The S3TC encoder

The library computes in doubles but runs under PC_24: every operation rounds to a 24-bit mantissa
while keeping the double exponent (its power iteration reaches about 1e139, far past float range).
Two best-ratio denominators live in initialised data and carry over between blocks; the
single-colour table is built on first use. The DXT5 alpha fit reads its caller's uninitialised
index array on its first pass; the port starts from zeros (the engine never encodes DXT5).

### IShow and the DirectShow base classes

Kept: Release closes the file on every call; Pause of an Ogg stream is a Stop that keeps the
decoder position; Bink's SetPosition inverts GetPosition's conversion wrongly (right only for 0);
the whole-buffer Ogg fill ignores looping; only a refill that wraps past the buffer's end marks the
end of a non-looping stream (the refill up to the play cursor writes silence without marking it,
so the stream stops later); the DirectShow graph is kept across Open calls; the volume is
866 · ln(v). Not copied: Close returns what freeing the Ogg decoder left in EAX (1 with the
original's C runtime) when there was a decoder; the port returns 0. Fixed: leaked streaming
buffers, notify interfaces, thread and file handles on Close and failures; negative `ov_read`
counts; a stale "AVI Splitter" pointer; `CBaseRenderer::FindPin`'s AddRef through null; empty tile
lists.

The base classes are the DirectX 9 SDK's built without DEBUG. The binary differs from the
published source in places (SendQuality's proportion in multiples of 1000, isqrt's zero check,
CEnumPins::Next's count handling, CRendererPosPassThru::RegisterMediaTime using GetTime, FindPin
without a null check); the port follows the binary.

### ISND

Kept: the voice manager stores a suspended sound's play cursor in bytes and reads it as
milliseconds, reads a stolen voice's cursor from the new sound's buffer, never checks the last
suspended entry in its clean-up loop, prefers the louder sound on equal priority, and drops
DSBPLAY_LOOPING when resuming after a pause. ISND_sound's flags and ISND_source's format are unset
until Open. Fixed: leaks on failure paths, the WAV "data" chunk search looping for ever, divisions
by a zero block alignment or length.

### I3D frames, sectors, models, animation

Kept: SetWorldPosRot ignores the sector's cached inverse; LinkTo's occluder pass (its mask 0x800
matches nothing); loaded lights shine without the factor 0.5 the colour setters apply; a light's
SetOn clears no parent bounds; point lights do not tell their sectors about a type change; the
scene rings keep stale links after an object leaves them. Fixed: Duplicate of a frame with itself
(read its freed name); RebalanceNameTree's stale left link; Update's target list pointer; a null
write in the joint code; an area's LinkTo, which in the original loops for ever at a sector
without a scene and reads through null above the top frame.

### Buffer managers and meshes

MSVC 6's `std::multiset` is kept (`src/core/multiset.h`): the managers order their buffers by free
space, which changes while a buffer is in the tree, so where a new buffer lands depends on the
tree's exact shape. Kept: an empty allocation shares its offset with the next one (freeing by
offset can free the other's block); a level's Release deletes a plain block when it has no
vertices but a buffer pointer; FillRenderVB writes 0x28-byte vertices into a 0x20-stride buffer
(render flag 0x10) and 16-byte ones into a 0x18 one; extra data keeps the vertex count it was made
for; SetLevel(null, i) cuts the counted levels at i. Fixed: RemoveFaceGroup of a group that is not
first (the original truncated the list), a level's Duplicate leaving a deleted face group list,
the mesh's Release leaking levels past its count, Duplicate leaking on failure, GetStats and face
group Unlock reading through null, ComputeBounds of an empty buffer looping 2^32 times.

### The lightmap baker

Construct(scene, plane normal, texels per unit, LMCONS_* flags, progress callback, context); the
callback gets the loader's message 8 with states 0, 1 (progress), 3, 4, 5, 6. Driver state 0x28 is
the samples per texel axis; light modes 0x02 (shines into lightmaps) and 0x20 (traced for
shadows). Kept: the face grouping rescans from face 1 after each addition; the outside-sample sum
only grows on a new minimum; the blur's 255 clamp tests the truncated byte. Fixed: an object
without a sector above it; a missing plane normal; LMCONS_ONE_PATCH on several levels (patched
again for ever without a callback); unset rectangles; an unchecked allocation; lightmap vertices
past their arrays; unset face patches; light types above 7; the original's limit of 100 face
groups in a local array.

### Collision

MoveSphere's sweep test: the first touching edge (v0–v1, then v2–v1, then v0–v2) decides; a
contact no nearer than the best skips the face, else it is the face's contact and a hit. The
original's ray code writes the caller's colour as 16 bytes when an opaque face ends an all-hits
query, keeps part of each query's state for the next, and hands the hit callback an unset normal
for a box an all-hits unlimited ray misses.

### The loader

The game's 6,200 model, shadow and mission files load identically. Unset in the original after a
load (its values are leftovers from its heap or stack): the frames' scaleW for frames it made
rather than loaded, a skin's parents past its joint count, the boxes' w words, a light's effective
colours' w (its Load copies 16 bytes of a stack temporary), a sound's and an emitter's velocity w,
words nothing writes, and what only the renderer or the application sets (visual depth, billboard
matrix, portal rectangles, occlusion volumes, mirror reflections, shadow boxes, the scene's
projection and weather matrices). I3D_area's Load reads nothing (not even the frame's own data).
Kept: a file with more than 6,000 materials is refused with the file left open. Fixed: a model file
with more than 10,000 frames overran the frame array; a skinned mesh read all of a file's joint
parents into a 64-byte array, kept a released skin pointer when the file had no skin data, and
made room for a level's vertices before reading how many it weights; a lens element without a
material ID took the previous element's (unset for the first, shared without a reference); a
portal with more than 8 vertices overran the portal.

## How the tests work

- Most tests load the original DLL and drive its objects through their vtable slots next to the
  port's (internal `__thiscall` functions are called as `__fastcall(self, nullptr, ...)`), on a
  fake Direct3D 8 device (`tests/fake_d3d8.h`) or fake DirectSound objects; both sides'
  `timeGetTime` imports can be patched to one clock.
- `compare_loader` loads the original and the port's built DLL side by side. It reads the port's
  class layouts from its PDB (DbgHelp), walks both object graphs from each loaded model member by
  member, pairs pointers one to one and matches classes by their vtables.
- A new test should be mutation-checked: break one line of the port and see the test fail.
