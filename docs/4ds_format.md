# The .4DS model format

`.4ds` files hold the models and mission scenes of Hidden & Dangerous 2 (LS3D engine 5.59):
materials, then a hierarchy of frames — meshes, skinned meshes, lights, sectors with their
portals, dummies, joints and so on. The engine reads them in `src/i3d/load.cpp`
(`OpenModelFile` → `ReadModelFile`, meshes in `LoadMesh`) and each frame type's `Load` method.
This document describes format version 0x29, the only one the engine accepts.

The layout below was checked with a reader written from this document alone: it reads all
6,190 `.4ds` files the game ships (5,946 in `Models\`, 244 in `Missions\`) exactly to their last
byte.

Animations live in a separate [.5ds file](5ds_format.md) of the same name.

## Conventions

All values are little-endian.

| Type | Size | Meaning |
|---|---|---|
| `u8`, `u16`, `u32` | 1, 2, 4 | unsigned integers |
| `i32` | 4 | signed integer |
| `f32` | 4 | IEEE single-precision float |
| `vec3` | 12 | `f32` x, y, z |
| `vec4` | 16 | `f32` x, y, z, w (the w of vertices and colours is not used) |
| `quat` | 16 | rotation: `f32` x, y, z, w (w last) |
| `string` | 1 + n | `u8` length n, then n bytes, no terminator (at most 255) |
| `box` | 32 | `vec3` min, `f32` (unused), `vec3` max, `f32` (unused); an empty box has min > max |
| `bound` | 48 | `box`, then the sphere around it: `vec3` centre, `f32` radius |
| `face` | 6 | three `u16` vertex indices |
| `matrix` | 64 | 4 × 4 `f32`, row by row: the x, y and z axes, then the position (Direct3D style, row vectors) |

- Coordinates are Direct3D's: left-handed, +X right, +Y up, +Z forward.
- A frame's position, rotation and scale are in its parent's space. The engine builds the
  rotation matrix with m[0][1] = 2xy − 2zw: D3DX (`D3DXMatrixRotationQuaternion`) needs the
  conjugate (−x, −y, −z, w) for the same matrix.
- Material IDs and frame references are 1-based indices into the file's materials and frames;
  0 means none.

## Layout

```
header
u16       material count
material  × count
u16       frame count
frame     × count
u8        animation flag
```

### Header (14 bytes)

| Offset | Type | Value |
|---|---|---|
| 0 | `char[4]` | `"4DS\0"` (else I3DERR_FILECORRUPTED) |
| 4 | `u16` | version, 0x29 (else I3DERR_BADVERSION) |
| 6 | `u8[8]` | a Windows FILETIME (the exporter's time stamp; the loader does not check it) |

### Materials

At most 6,000 (more: I3DERR_LIMITEXCEEDED, nothing else is read). Each material:

| Type | Field |
|---|---|
| `u32` | flags (below) |
| `vec4` | diffuse colour |
| `vec4` | ambient colour |
| `vec4` | specular colour |
| `vec4` | emissive colour |
| `f32` | specular power |
| `f32` | opacity (below 1: blended) |
| `f32` | *only with flag 0x00080000:* the environment texture's blend factor (with blend mode 1 below) |
| `string` | *only with flag 0x00080000:* the environment texture's file name |
| `string` | the texture's file name (empty: none) |
| `string` | *only when the texture name is not empty, flag 0x40000000 is set and none of 0xA1000000:* the opacity map's file name (empty: the texture's own alpha) |
| `u8` | *only with flag 0x04000000 (animated):* frame count of the texture sequence |
| `u8` | *animated:* frame count of the environment texture sequence |
| `i32` | *animated:* loop start frame of the texture sequence (negative: a random frame each switch) |
| `i32` | *animated:* milliseconds per frame |
| `i32` | *animated:* loop start of the environment sequence |
| `i32` | *animated:* its milliseconds per frame |

File names have no directory: the engine looks for them in the game's texture folders, then in
`Maps\`. A `.dds` file keeps its own format and mip levels; BMP and TGA files are converted to the
device's formats. An animated material with 2 or more frames loads the numbered files of its
sequence: the two characters before the extension are replaced by `00`, `01`, … (`fire00.bmp`,
`fire01.bmp`, …; the opacity map's name too with flag 0x02000000).

Material flags:

| Bits | Meaning |
|---|---|
| 0x0000007F | environment coordinates' scale: the generated coordinates are scaled by 2√2 / this value |
| 0x00000080 | drawn additively (as 0x80000000) |
| 0x00000700 | how the environment texture combines: 1 blended by the factor above, 2 by the alpha so far, 3 multiplied, 4 multiplied × 2, 5 added |
| 0x00007000 | where the environment coordinates come from: 2–4 two axes of the position transformed by the renderer's environment matrix (x z, y z, x y), 5 the mesh's extra per-vertex data |
| 0x00008000 | the texture keeps a 32-bit copy of its first level's pixels in memory |
| 0x00010000 | texture coordinates clamped in u |
| 0x00020000 | … in v |
| 0x00040000 | has a texture |
| 0x00080000 | has an environment texture (the fields marked above are present) |
| 0x00100000 | the texture is always 32-bit |
| 0x00200000 | the texture is never DXT-compressed |
| 0x00400000 | the texture is never loaded at half size |
| 0x00800000 | the texture gets mip maps (when the driver allows them) |
| 0x01000000 | the alpha comes from the texture itself (TGA / DDS alpha) |
| 0x02000000 | an animated sequence also numbers its opacity maps |
| 0x04000000 | animated texture sequences (the fields marked above are present) |
| 0x08000000 | drawn with its own colours (else a white default material) |
| 0x10000000 | two-sided (drawn without culling) |
| 0x20000000 | colour key: the texture's 1-bit alpha (palette entry 0 is transparent) |
| 0x40000000 | the texture has alpha (an opacity map, or its own with 0x01000000) |
| 0x80000000 | drawn additively |

### Frames

At most 10,000 (more: I3DERR_LIMITEXCEEDED). Frames come in hierarchy order: a frame's parent
is always an earlier frame. Each frame:

| Type | Field |
|---|---|
| `u8` | frame type (below) |
| `u8` | *only for type 1 (visual):* the visual type (below) |
| `u16` | *only for type 1:* visual flags (below; the low 16 bits) |
| `u16` | parent: 1-based index of an earlier frame; 0: the model or the scene's primary sector |
| | the common frame data (all types but 14, area) |
| | the type's own data |

Frame type 0, an unknown visual type or a type above 0x10 stops the load (I3DERR_GENERIC /
I3DERR_FILECORRUPTED); so does a parent index that is not an earlier frame.

Common frame data:

| Type | Field |
|---|---|
| `vec3` | position |
| `quat` | rotation |
| `vec3` | scale |
| `f32` | (a fourth scale value, kept but not used) |
| `u8` | frame flags: 0x01 the frame is on (shown, active); 0x80 its position is locked. The loader clears 0x04, 0x10 and 0x20 and sets 0x08 (cache states). The game's files use 0x09 (on) and 0x08 (off), with leftover cache bits (0x11, 0x1D, 0x3D) |
| `string` | name. A model's frames get the model's name as a prefix ("model.frame") when the model has one |
| `string` | user properties: text the game reads (vehicle parts, damage zones, …) |

#### Frame types

| Type | Name | Data after the common frame data |
|---|---|---|
| 1 | visual | by visual type, below |
| 2 | light | [light](#light) |
| 3 | camera | none |
| 4 | sound | none (the game gives the sound its file and settings) |
| 5 | sector | [sector](#sector-and-portals) |
| 6 | dummy | `box`: an invisible box in the frame's space |
| 7 | target | [target](#target) |
| 8 | user | none |
| 9 | model | none (a model inside the model) |
| 10 | joint | `i32`: the joint's number in its skeleton (see [skin](#skin)) |
| 11 | volume | none |
| 12 | occluder | [occluder](#occluder) |
| 13 | scene | none |
| 14 | area | nothing at all, not even the common frame data (the engine does not support it: it logs "nepodporovany typ") |
| 15 | shadow | none |

The game's files use types 1, 5, 6, 7, 10 and 12 only.

#### Visual types

| Type | FourCC | Class | Data after the common frame data |
|---|---|---|---|
| 0 | `OBJ_` | object | [mesh](#mesh) |
| 1 | `LMAP` | lit object (lightmapped) | [mesh](#mesh) |
| 2 | `SNGM` | skinned mesh | [mesh](#mesh), then (when it has a mesh) [skin](#skin) |
| 3 | `SMMR` | skinned morph | [mesh](#mesh), [skin](#skin), [morph](#morph) |
| 4 | `BBRD` | billboard | [mesh](#mesh), then `u32` axis (0 x, 1 y, 2 z) and `u8` mode (0 about the axis and tilted toward the camera, 1 about the axis only, 2 facing the camera with the frame's scale, 3 facing the camera, 4 the frame's rotation in camera space, grown with distance) |
| 5 | `MRPH` | morph | [mesh](#mesh), [morph](#morph) |
| 6 | `LENS` | lens flare | [lens](#lens-flare) |
| 7 | `PROJ` | projector | [projector](#projector) |
| 8 | `MIRR` | mirror | [mirror](#mirror) |
| 9 | `EMTR` | particle emitter | [emitter](#particle-emitter) |
| 10 | `LNDP` | landscape patch | not in this engine: the load stops (I3DERR_GENERIC) |
| 11 | `SHD` | shadow visual | none |

The game's files use visual types 0, 2, 3, 4, 5, 6 and 8.

Visual flags (the bits the files use):

| Bit | Meaning |
|---|---|
| 0x0010 | the mesh's levels 1… are texture levels: level 0's geometry with other texture coordinates |
| 0x0040 | the geometry is in world space (moving the frame keeps the bounds) |
| 0x0080 | LOD distance measured from the frame's position, not the bound's centre |
| 0x0100 | drawn with a z bias |
| 0x0200 | receives shadows (with 0x0400: both shadow kinds) |
| 0x0800 | an object: takes its hierarchy's light level and alpha (set on all of the game's objects) |
| 0x1000 | skipped by collision queries asked to skip flagged visuals |
| 0x2000 | receives projectors' polygons (headlights, decals; with 0x4000 both kinds) |
| 0x8000 | its sector's fog does not apply |

### Animation flag

The last byte: non-zero means the file has an animation. After loading the frames the engine
opens the [.5ds file](5ds_format.md) of the same name and plays it on the model (or scene),
looping, on animation stage 0.

## The frame types' data

### Mesh

Object-like visuals (types 0–5) read a mesh:

| Type | Field |
|---|---|
| `u16` | 1-based index of an earlier visual whose mesh this one shares (0: a mesh of its own follows) |
| `u8` | *own mesh:* level count, at most 10 (0: no mesh; more than 10: I3DERR_LIMITEXCEEDED) |
| | *own mesh:* the levels |

Each level (detail levels, most detailed first):

| Type | Field |
|---|---|
| `f32` | LOD distance, squared: the level is drawn while (camera distance / (LOD scale × the frame's world scale))² is at most this. Past the last level's distance (when above 0) nothing is drawn; a single level with 0 is always drawn |
| `u32` | extra per-vertex data format (0: none; below) |
| `u16` | vertex count n; 0xFFFF: a *texture level* (below) |
| `vertex` × n | position `vec3`, normal `vec3`, texture coordinates `f32` u, v (32 bytes) |
| `u8` × (stride × n) | *only with an extra format:* the extra data |
| `u8` | face group count |
| face groups | |

Texture level: instead of vertices, `f32` u, v for each of **level 0's** vertices; the level's
face groups index level 0's vertices (with visual flag 0x0010 the game's objects switch textures
per distance this way). Its extra data, if any, also counts level 0's vertices.

Extra data formats (bytes per vertex): 0x01 adds 4, then 0x02 adds 8, or else 0x04 adds 16, or
else 0x08 adds 24 (only the first of 0x02 / 0x04 / 0x08 counts). With 0x01 the level is drawn
from its positions and these 4 bytes instead of the normals and texture coordinates; with 0x02
the 8 bytes are a second set of texture coordinates, used for the environment texture when its
material says so (flags 0x5000). The game's files use no extra data and no texture levels.

Each face group:

| Type | Field |
|---|---|
| `u16` | face count |
| `face` × count | |
| `u16` | material ID (0: none; past the material count: I3DERR_FILECORRUPTED, and the level's remaining groups are not read) |

### Skin

Skinned visuals (`SNGM`, `SMMR`) follow their mesh with:

| Type | Field |
|---|---|
| `u8` | joint count J (0: no skin data, nothing else) |
| `box` | the mesh's box in its bind pose |
| `u8` × J | each joint's parent joint (1-based; 0 none) |
| J × (`matrix`, `box`) | each joint's bind-pose inverse (mesh space to joint space) and the box of the vertices it moves, in its space (empty: none) |
| per mesh level: `u32` W, then W × (`u8`, `u8`) | the level's first W vertices: their joint (1-based; 0 none) and the joint's parent's weight × 256 (0: the joint alone). The other vertices do not move |

The joints are the joint frames (type 10) below the visual — down to the next skinned visual —
joint *k* (1-based) being the joint frame whose number is *k* − 1. At most 64 joints are used.

### Morph

`MRPH` and `SMMR` visuals follow their mesh (and skin) with morph targets:

| Type | Field |
|---|---|
| `u8` | target count T (0: no morph data, nothing else) |
| `u8` | channel count C |
| `u8` | level count L |
| L × C channels | level by level, channel by channel |
| `bound` | the morphed mesh's bounds (used as the visual's) |

Each channel:

| Type | Field |
|---|---|
| `u16` | vertex count V (0: nothing else) |
| V × T × (`vec3`, `vec3`) | for each vertex, each target's position and normal |
| `u8` | 1: vertex indices follow |
| `u16` × V | *with indices:* the mesh level's vertex each one replaces (without: vertices 0…V − 1) |

A morphed vertex is the targets' positions and normals blended by the channel's weights (target
0 has weight 1 at the start); it replaces the position and normal of its mesh vertex.

### Lens flare

| Type | Field |
|---|---|
| `u8` | element count |
| per element: `f32`, `f32`, `u16` | position along the flare's axis from the screen's centre, size (times the texture's), material ID (0: the previous element's material) |

### Projector

| Type | Field |
|---|---|
| `u8` | 1 orthogonal, 0 perspective |
| `u8` | how the colour fades over the depth (0, 4, 5, 8, 9) |
| `u16` | material ID (0: none) |

The projected volume is x, y −1…1, z 0…1 in the frame's space.

### Mirror

| Type | Field |
|---|---|
| `bound` | the mirror's bounds |
| `matrix` | the mirror plane in the frame's space (its z axis is the normal) |
| `vec4` | the surface's colour |
| `f32` | range: reflects what is within it (times the LOD scale) |
| `i32` | vertex count V |
| `i32` | face count F |
| `vec4` × V | the mirror's vertices |
| `face` × F | |

### Particle emitter

| Type | Field |
|---|---|
| `u32` | texture flags, as a material's flags |
| `string` | the particles' texture (empty: none, nothing else) |
| `string` | *only with flag 0x40000000 and none of 0xA1000000:* the opacity map |

The particles' other settings come from the game.

### Light

0x60 bytes:

| Offset | Type | Field |
|---|---|---|
| 0x00 | `vec4` | colour |
| 0x10 | `vec4` | specular colour |
| 0x20 | `vec4` | ambient colour |
| 0x30 | `u32` | mode: 0x02 shines into lightmaps, 0x08 the light the shadow visuals use, 0x20 its light is blocked by faces in lightmaps |
| 0x34 | `u32` | type: 1 point, 2 spot, 3 directional, 4 ambient, 5–7 fog |
| 0x38 | `f32` | power |
| 0x3C | `f32` | specular power |
| 0x40 | `f32` | ambient power |
| 0x44 | `f32` | range (fog: where the fog is full) |
| 0x48 | `f32` | spot falloff |
| 0x4C | `f32` × 3 | attenuation: constant, linear, quadratic (fog: the first is where the fog starts) |
| 0x58 | `f32` | spot cone, inner angle (radians) |
| 0x5C | `f32` | spot cone, outer angle |

A loaded light shines with colour × power (the engine's colour setters halve it; loaded lights
are not halved).

### Sector and portals

A sector is a convex room of a scene; portals lead out of it.

| Type | Field |
|---|---|
| `u32` | sector flags (below) |
| `i32` | environment ID (I3D_sector::SetEnvironment) |
| `i32` | vertex count V |
| `i32` | face count F |
| `box` | the sector's box, in its space |
| `vec4` × V | the hull's vertices |
| `face` × F | the hull's faces |
| `u8` | portal count |
| portals | |

Sector flags in the files: 0x001 (always set), 0x040 the sector builds an occluder from its
hull, 0x100 seen through a far portal, only the sector's direct children are drawn (not their
children), 0x800 its sound environment is new. The game's files use 0x801, 0x841, 0x901, 0x921
and 0x941; the loader clears 0x020.

Each portal:

| Type | Field |
|---|---|
| `u8` | vertex count N |
| `vec3`, `f32` | the portal's plane: normal and distance (the sector's side in front) |
| `u32` | portal flags: 0x04 open, 0x10 not looked through beyond its range (else looked through as "far"), 0x20 cuts its sector in two; 0x40 is set on many of the game's portals but not used by the engine |
| `f32` | range: the squared camera distance (divided by the field of view's scale, squared) beyond which the portal is far (0: never) |
| `f32` | a second range (kept, not used) |
| `u32` | (unused) |
| `vec4` × N | the portal's vertices, in the sector's space |

The engine keeps at most 8 vertices (the game's portals have 3–8).

### Dummy

`box`: an invisible box in the frame's space.

### Target

| Type | Field |
|---|---|
| `u16` | mode: 0x1 the frames point their z axis at the target; 0x2 / 0x4 / 0x8 they copy its rotation / position / scale |
| `u8` | frame count |
| `u16` × count | 1-based frame indices (0 is skipped) |

The indices may point to any frame of the file, later ones included: the loader resolves them
after reading all frames (an index past the last frame: I3DERR_FILECORRUPTED). No target in the
game's files lists frames.

### Occluder

| Type | Field |
|---|---|
| `i32` | vertex count V |
| `i32` | face count F |
| `vec4` × V | |
| `face` × F | |

An occluder hides what is behind it (its faces should form a closed volume).

## Loading

- `I3D_model::Open` and `I3D_scene::Open` load the file named by the caller with its extension
  replaced by `.4ds`. Frames without a parent are linked to the model, or to the scene's primary
  sector. Mission scenes (`Missions\<mission>\SCENE.4ds`) load into the scene; the missions'
  `map.4ds` and `loader.4ds` and everything in `Models\` load as models.
- While a named model loads, its frames stay out of the driver's frame-name index; afterwards
  they are renamed "model.frame".
- Skinned visuals with visual flag 0x0800 find their joints after the whole file is read.
- A failure keeps what was read so far and returns the first error: I3DERR_NOFILE,
  I3DERR_FILECORRUPTED, I3DERR_BADVERSION, I3DERR_LIMITEXCEEDED, I3DERR_OUTOFMEM, I3DERR_GENERIC.

## The game's files

| | Count |
|---|---|
| files | 6,190 (2,314 with the animation flag) |
| materials | 37,492; 33 animated, 74 with an environment texture |
| visuals | 168,653 objects, 91 skinned meshes, 82 skinned morphs, 24 morphs, 108 billboards, 149 lens flares, 2 mirrors |
| meshes | 168,425 levels; 21,854 visuals share an earlier one's mesh |
| other frames | 47,130 joints, 16,046 dummies, 2,905 sectors with 7,801 portals, 1,480 occluders, 291 targets |
| user properties | on 26,699 frames |

One shipped file is damaged in a harmless way: `Models\m_L3_bed02Crash.5DS` (see the
[.5ds document](5ds_format.md)).
