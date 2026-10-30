# The .5DS animation format

`.5ds` files hold keyframe animations: for each animated frame of a model, rotation, position and
scale keys and event notes. The engine loads one into an animation set (`I3D_animation_set::Open`,
which calls `OpenAnimationFile` in `src/i3d/load.cpp`; the tracks are read in `src/i3d/anim.cpp`)
and plays it on a model or scene, matching tracks to frames by name. A [.4ds file](4ds_format.md)
whose animation flag is set loads the `.5ds` of the same name with it. This document describes
format version 0x7A, the only one the engine accepts.

The layout below was checked with a reader written from this document alone on the 2,315 `.5ds`
files the game ships: each one's data is fully accounted for (only alignment padding, at most
15 bytes a file, is left over), apart from one damaged file described at the end.

## Conventions

As in the [.4ds document](4ds_format.md): little-endian; `u16`, `u32`, `f32`; `vec3` = three
`f32`; `quat` = `f32` x, y, z, w (w last; D3DX wants the conjugate). Positions, rotations and
scales are in the frame's parent's space, like a frame's own position, rotation and scale.

## Layout

```
header (18 bytes)
data   (the size the header gives)
```

| Offset | Type | Value |
|---|---|---|
| 0 | `char[4]` | `"5DS\0"` (else I3DERR_FILECORRUPTED) |
| 4 | `u16` | version, 0x7A (else I3DERR_BADVERSION) |
| 6 | `u8[8]` | a Windows FILETIME (the exporter's time stamp; not checked when loading) |
| 14 | `u32` | the data's size in bytes |
| 18 | | the data |

The engine reads the data into a 16-byte aligned block and works on it in place: **every offset
below counts from the start of the data (file offset 18), and so do the alignments.**

### The data

| Offset | Type | Field |
|---|---|---|
| 0 | `u16` | track count N |
| 2 | `u16` | length in frames (the animation's length is this × 40 ms) |
| 4 | N × (`u32`, `u32`) | for each track: the offset of its name, the offset of the track |

Names are NUL-terminated frame names, without the model's prefix (the short names the `.4ds`
file stores), compared case-sensitively. When loading, the engine turns these offsets — and the
offsets of text notes (below) — into pointers.

In the game's files the tracks follow the table, starting at a 16-byte boundary, and the names
come last; the engine only follows the offsets, so any order works as long as the alignments
below hold. Tracks start on a 4-byte boundary.

### A track

| Type | Field |
|---|---|
| `u32` | flags: which sections follow |
| sections | in this order, each present when its flag is set |

| Flag | Section |
|---|---|
| 0x04 | rotation keys |
| 0x02 | position keys |
| 0x08 | scale keys |
| 0x10 | note keys with 16-bit values |
| 0x20 | note keys with texts |

A track has at most one of the note sections.

Every section starts with its key count and the key times:

| Type | Field |
|---|---|
| `u16` | key count K |
| `u16` × K | key times in frames (25 frames a second: frame × 40 ms), ascending |

Then the values:

- **Rotations:** padding to the next 16-byte boundary (from the data's start), then K × `quat`.
- **Positions, scales:** 2 bytes of padding when K is even (so the values start 4-byte aligned),
  then K × `vec3`.
- **16-bit notes:** K × `u16`, right after the times.
- **Text notes:** the same padding as positions, then K × `u32`: each the offset of a
  NUL-terminated text, counted from the end of this offset array (the texts follow it).

## Playing

- `I3D_animation_set::Open(name)` replaces the name's extension with `.5ds`; the set is named after
  the file (without folder and extension).
- `I3D_model::SetAnimation(set, stage, weight, mode)` (and the scene's) binds each track to the
  model's frame of the same short name; frames without a track keep their pose. A model plays up
  to 8 sets at once, in stages.
- Between two keys positions and scales are interpolated linearly, rotations by slerp. Before the
  first key a frame holds the first key's value, after the last key the last's. Stages are blended
  by their weights, then a pose (`I3D_pose`) can override or add.
- Modes: 1 plays once and stops at the end, 2 loops. A set opened by a `.4ds` file loops on stage 0.
- Note keys are events: when playback passes one, the frame's callback (if the game asked for
  them with `SetCallback`) gets message 0x200 with the 16-bit value, or 0x1000 with a pointer to
  the text, and the key's time in milliseconds. The game's texts are short codes on the root
  track `a` of characters (`u`, `s1`, `s2`, `0`, `1`, `w`, `t`, `l`, `@21`, …) and
  `fire_start` / `fire_end` on the `stihac` (fighter plane) track.

## The game's files

| | Count |
|---|---|
| files | 2,315 |
| tracks | 43,217: rotation only 27,519; rotation and position 11,135; rotation, position and scale 2,976; position only 1,092; with text notes 477; others 18 |
| text notes | 1,209 |
| 16-bit notes | none |

Seventeen sections have key times out of order. The engine's key search assumes ascending times:
it interpolates between the first key whose time is past the current frame and the key before it.

`Models\m_L3_bed02Crash.5DS` is damaged: its header gives a data size of 16 bytes but only 4
follow (0 tracks, 0 frames). The engine reads the 4 bytes that are there and gets an empty
animation.
