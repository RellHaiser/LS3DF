# The sound device (audio)

Everything the engine plays goes through one interface, `audio::Device` (`audio::g_pAudio`),
declared in [`src/audio/audio.h`](../src/audio/audio.h). This covers ISND (sources, sounds, 3D
sounds, the listener, EAX, the voice manager), IShow's Ogg Vorbis streaming and the I3D sound
frames. Its only implementation so far is the DirectSound backend in
[`src/audio/dsound`](../src/audio/dsound), which passes every call to DirectSound unchanged. The DLL
therefore makes exactly the DirectSound calls it made before.

This is the sound counterpart of the [render device](render_device.md), built the same way and for
the same reason: to make the API replaceable (OpenAL Soft is the planned successor) without
changing behaviour.

## Design

**The vocabulary stays DirectSound 8's.** The engine describes sound in DirectSound's terms:
buffer descriptions and wave formats, volumes and pans in hundredths of a decibel, play flags and
priorities, 3D modes, distances and cones, the listener's parameters, and EAX 3 reverb as property
sets. A backend has to reproduce that model. The types come from
[`src/dx/dsound_types.h`](../src/dx/dsound_types.h), which on Windows takes them from the SDK's
`dsound.h`. Creative's EAX declarations stay in `src/isnd/eax.h`.

**Objects are opaque handles.** The DirectSound object, buffers, 3D buffers, the listener, EAX
property sets and position notifications are `audio::System`, `Buffer`, `Buffer3D`, `Listener`,
`PropertySet` and `Notify`. These are incomplete types that only a backend looks inside, and
operations on them go through the device (`g_pAudio->Play(buffer, ...)`). DirectSound's
`QueryInterface` becomes one `Query` overload per interface the engine asks for, and like
`QueryInterface` each gives a new reference. The handles sit where the DirectSound pointers were and
have the same size:

| Structure | Members |
|---|---|
| `ISND_driver` | `dsound` (+0x0C), `eax` (+0x10), `eaxBuffer` (+0x14), `primary` (+0x28), `listener` (+0x2C) |
| `ISND_source` | `buffer` (+0x30) |
| `ISND_sound` | `buffer` (+0x08) |
| `ISND_3Dsound` | `buffer3D` (+0x20) |
| `IShow` | `notify` (+0x6C), `buffer` (+0x70) |
| the voice manager's `S_voice`, `S_suspendedVoice` | `buffer` (+0x04) |

The DirectSound backend's handles are the DirectSound interfaces themselves
(`audio::dsound::DS()` / `Handle()` convert them), so these members hold the same values as before.
A buffer handle holds whichever buffer interface the engine had (DirectSound 8's extends the
original); the backend calls only the original's methods on it.

**One escape hatch.** Bink plays its own sound through DirectSound, so IShow hands it the device's
own object (`Native()`). The video seam will replace this.

**The boundary is checked.** `python build.py` (and `check`) fails when code outside `src/dx` and
`src/audio/dsound` includes `dsound.h` or the backend, or names a DirectSound interface or
function. Comments may still name them.

## What a new backend has to provide

Everything in `audio::Device`:

- **The device:** enumeration (for the log), creation (the EAX Unified class or the default device),
  capabilities (secondary buffer rate limits and the free hardware 3D buffers the voice manager
  counts), speaker configuration.
- **Buffers:** the primary buffer's format; secondary buffers from PCM data, static (whole files),
  duplicated (several sounds share one source's data) or streamed (IShow's Ogg player refills a
  ring buffer on position notifications). Volume, pan, frequency, play / stop / position and
  status, play priorities and the voice-placement flags.
- **3D:** per-buffer position, velocity, cone, distances and mode (normal, disabled); one
  listener with position, orientation, velocity and distance, Doppler and rolloff factors.
- **EAX 3:** the listener property set (all parameters at once, or single properties) and support
  queries for the listener and buffer sets.

## Tests

- `compare_isnd` compiles the backend (`// test-sources: src/audio/dsound/audio_dsound.cpp`) and
  puts its fake DirectSound objects into the drivers' members as handles. It still compares every
  DirectSound call, its arguments, the return values, the objects' fields and the voice manager's
  lists against the original: 100,000 rounds identical on each of four seeds (1 to 4). A deliberately
  broken backend (`SetPan` calling `SetVolume`) shows 21 differences in 20,000 rounds.
- `compare_texture` compiles IShow, so it compiles the backend too.
- The rest of the suite gives the same results as before the change.

## Not covered

Like before, device creation, EAX detection and enumeration need real hardware and are not
compared, and IShow's streaming only compiles.
