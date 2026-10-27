# Going cross-platform

The long-term goal: LS3DF (and with it the game) building and running on operating systems other
than Windows. Direct3D 8, DirectSound, DirectInput, DirectShow, Bink and Win32 give way to
portable libraries:

| Today | Replacement |
|---|---|
| Win32 window, messages, clock, threads | SDL3 |
| DirectInput 8 | SDL3 |
| DirectSound 8 + EAX 3 | OpenAL Soft (EAX reverb through EFX) |
| Bink 1, DirectShow | FFmpeg |
| Direct3D 8 | SDL_GPU (Vulkan, Metal, Direct3D 12 underneath) |

The 32-bit Windows build on Direct3D 8 stays as the reference: the differential tests compare it
with the original DLL, and each new backend is checked against it before the old one goes.

## The phases

| Phase | What | Status |
|---|---|---|
| 1 | Seams: every API behind an engine-owned interface, still on Direct3D 8 | **done** |
| 2 | SDL3 (window, input, time), OpenAL Soft (sound) and FFmpeg (video) backends, still rendering with Direct3D 8 | next |
| 3 | An SDL_GPU render backend on Windows, its output compared pixel by pixel with the Direct3D 8 backend's | |
| 4 | A 64-bit build, together with the game's port | |
| 5 | Linux, then macOS | |

### Phase 1: the seams (done)

Each seam keeps the original API's vocabulary, holds opaque handles where the API pointers were
(the game reads the engine's structures, so layouts stay), puts the API calls in one Windows
backend, and is enforced by `python build.py`. The DLL makes the same API calls as before; the
differential tests showed no change.

| Seam | Interface | Windows backend | Commit | Details |
|---|---|---|---|---|
| Rendering | `gfx::Device` | `src/gfx/d3d8` | d2c3b93 | [render_device.md](render_device.md) |
| Sound | `audio::Device` | `src/audio/dsound` | 80c81d1 | [sound_device.md](sound_device.md) |
| Input | `input::Device` | `src/input/dinput` | aefd06e | [input_device.md](input_device.md) |
| Window, messages, time, threads, events | `platform::` functions | `src/platform/win32` | 47159f2 | [platform.md](platform.md) |
| Video | `video::Device` | `src/video/win32` | 0a59157 | [video_device.md](video_device.md) |

New differential tests came with the seams: `compare_input`, `compare_window` and
`compare_show`. They found two bugs of the port, both fixed: IGraph's held-key repeat started at the
wrong value, and the port marked the end of an Ogg stream too early; see [porting_notes.md](porting_notes.md).

The built DLL then ran in the game for the first time (October 2026): menus, two cooperative
multiplayer missions, music, the intro and credits movies.

Left on Win32 for now, small and only needed for another platform
([platform.md](platform.md#not-covered-yet)): the DirectX 8.1 check, the OS name in the log, COM
start-up, `RECT` helpers, the texture directories' enumeration, resource lookups and `MessageBox`,
the INI path from the module name, the command line.

Known escape hatch: Bink plays its own sound through DirectSound, handed the sound device's object
(`audio::Device::Native`); an FFmpeg backend plays its sound through the sound device instead.

### Phase 2: SDL3, OpenAL Soft, FFmpeg (next)

Each new backend sits beside the Windows one and is chosen at build time (the platform) or start-up
(the devices), so the two can be compared in the same tests:

- **Platform and input on SDL3:** the window, messages and the decisions IGraph makes on them
  (`platform::WindowEvents`), the clock, threads, locks and events; the keyboard and mouse with
  DirectInput's key codes (DIK) kept as the engine's vocabulary.
- **Sound on OpenAL Soft:** buffers, streaming with position notifications, 3D sound and the
  listener, EAX 3 reverb mapped to EFX.
- **Video on FFmpeg:** Bink 1 movies stepped frame by frame (the `video::Device` movie model), its
  sound through the sound device; the DirectShow graphs (AVI and others) on FFmpeg too.

Open questions: how close OpenAL's EFX reverb gets to EAX 3; how well FFmpeg decodes Bink 1 (the
game's movies are Bink 1.x).

### Phase 3: SDL_GPU

A render backend on SDL_GPU behind `gfx::Device`: fixed-function Direct3D 8 states turned into
shaders, its frames compared pixel by pixel with the Direct3D 8 backend's.

### Phase 4: 64 bits

The engine and the game's port built as 64-bit. The x87 bit-exactness the tests rely on ends here,
which is why the 32-bit build stays as the reference.

### Phase 5: other systems

Linux, then macOS.
