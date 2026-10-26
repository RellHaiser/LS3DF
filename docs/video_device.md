# The video device (video)

IShow plays its videos through one interface, `video::Device` (`video::g_pVideo`), declared in
[`src/video/video.h`](../src/video/video.h). Its only implementation so far is the Windows backend
in [`src/video/win32`](../src/video/win32). That backend is IShow's Bink and DirectShow code, moved
there unchanged, with the texture renderer filter. The DLL therefore makes exactly the Bink and
DirectShow calls it made before.

This is the last of the device seams (after [render](render_device.md), [sound](sound_device.md),
[input](input_device.md) and the [platform](platform.md)). FFmpeg is the planned successor for
the movies.

## Design

**Two kinds of player.** IShow drives its videos in two different ways, and the device keeps
both:

- **Movies** (`.bik`, the game's videos) are pulled. IShow steps through the frames itself: it
  asks whether the next frame is due, decodes it, copies it into its tiles and moves on. The
  vocabulary is Bink's: frame numbers from 1, the frame rate as rate / divisor, a frame copy's
  destination formats and its "copy everything" flag, volumes from 0 to 32,768. A decoder that
  can be stepped (FFmpeg) fits this model.
- **Graphs** (`.avi` and anything else) run by themselves, DirectShow's way. The device creates,
  runs, pauses, stops and seeks a graph, sets its volume (hundredths of a decibel), reports its
  events (complete, user abort, paused) and the picture size. An `.avi`'s frames are pushed into
  IShow's tiles; other files are shown by the player's own renderers.

Ogg Vorbis music is not part of the device: vorbisfile is portable, and the stream already plays
through the sound device.

**Frames come in through a sink.** The texture renderer used to call into IShow. IShow cannot
implement a backend's interface (its virtual table is the game's ABI), so the device takes a
`video::FrameSink` instead. IShow's adapter (in `ishow.cpp`) answers what the original renderer
asked IShow:

| `FrameSink` | IShow's answer (the original renderer's code) |
|---|---|
| `FrameFormat` | 32-bit with `ISHOW_TRUECOLOR` and a true-colour texture format, else the 16-bit format's layout (565 / 555) |
| `SetSize` | creates the tiles and returns their texture format |
| `Frame` | copies the bottom-up frame into the tiles and counts it (`GetFrameCount`) |

A graph owns the sinks it was given and deletes them with itself.

**Objects are opaque handles** (`video::Movie`, `video::Graph`), operated on through the device.
They sit where IShow kept the API pointers:

| IShow member | Before | Now |
|---|---|---|
| `graph` (+0x04) | `IGraphBuilder` | `video::Graph` (the backend's record of the graph and its interfaces) |
| +0x08, +0x0C, +0x14, +0x18 | `IMediaControl`, `IMediaSeeking`, `IBasicAudio`, `IMediaEvent` | unused, in the graph record |
| `movie` (+0x1C) | `BINK` | `video::Movie` (the `BINK` with the Windows backend) |
| +0x50 | the texture renderer | unused, in the graph record |
| `thread` (+0x64), `event` (+0x68) | thread and event handles | `platform::Thread`, `platform::Event` |

The game reads IShow only through its virtual table and the tiles, so the unused members keep
their places and the object its size (0x8C).

**One escape hatch remains.** Bink plays a movie's sound itself, through DirectSound, so
`UseSound` hands it the sound device's own object (`audio::Device::Native`). The two Windows
backends meet there. A new video backend will play the sound through the sound device instead.

**The platform got events.** The Ogg stream thread waits for the sound buffer's position
notifications. `platform::Event` (`NewEvent`, `SetEvent`, `WaitEvent`, `DeleteEvent`) replaces
IShow's Win32 event, and `NativeEvent` gives DirectSound the handle it sets. IShow's threads,
sleeps and events all go through the platform now.

**The boundary is checked.** `python build.py` (and `check`) fails when code outside
`src/video/win32`, `src/dshow`, `src/dx` and `src/external` includes `dshow.h`, Bink or the backend,
calls a Bink function or names the DirectShow interfaces. The platform check no longer exempts
IShow; only the DirectShow base classes keep their Win32 calls, as part of the Windows backend.

## What a new backend has to provide

Everything in `video::Device`:

- **Movies:** open and close, the size, frame count, current frame and rate; pause, seek to a
  frame, "is the next frame due", decode, advance; copy a rectangle of the decoded frame in RGB32,
  RGB565 or RGB555 (all of it, or what changed); the volume; the sound device to play the sound on.
- **Graphs:** create and release; render a file with the player's own output, or with its video
  into a `FrameSink` (in the sink's pixel layout, bottom-up rows) and its sound to the sound card;
  run, pause, stop; seek and report the position (100 ns units); the volume; the next event; the
  size of the picture rendered into the sink.

## Tests

`compare_show` is new. It is the first test of IShow, so it covers the players themselves as well
as the seam. The original's players and the port's run the same operations:

- **Ogg:** the game's three shortest music files (from the unpacked `Sounds` folder), decoded
  into fake DirectSound buffers. The test wakes each side's stream thread as DirectSound's
  notification would and waits for its refill. The DirectSound calls, the buffers' bytes and the
  players' fields must match.
- **Bink:** the game's three smallest movies (`Gamemovie`), through the real `binkw32.dll`. Both
  sides' imports of Bink are pointed at logging wrappers. `BinkWait` gives both sides the same
  scripted answers, so the result does not depend on the clock, and `BinkSetSoundSystem` is only
  logged. The Bink calls with their arguments, the tiles (layout, quads, and the pixels on the
  fake Direct3D device) and the fields must match.
- **AVI:** uncompressed AVI files written by the test (24-, 32- and 16-bit, one with a sound
  track), through Windows' DirectShow and each side's texture renderer, on four texture formats
  and sizes. The graphs run without a reference clock, so every frame between a random start and
  stop reaches the tiles. A silent WAV goes through a plain graph. The frame counts, the last
  frame's pixels, the events, positions and sizes must match.

Result: 20,000 Ogg rounds, 20,000 Bink rounds and the AVI and WAV cases, identical on each of
two seeds (1 and 2). A run with seed 1 takes two minutes. It opens 1,445 music files and wakes
the stream threads 8,066 times (170 of the wakes stop a stream at its end). It opens 1,615 movies,
answers 4,796 `BinkWait` calls and compares 8.1 GB of tile pixels. All 12 AVI cases and the WAV
open (the AVI with sound needs a sound card).

18 deliberate bugs are caught:

- 7 in the backend's Bink half: the frame sought, the copied rectangle, the rate, the wait, the
  volume, the sound system and pause;
- 4 in its graph half: seeking, the picture size, events and rendering a file;
- 1 in the texture renderer's pixel format;
- 2 in IShow's frame sink: the copy width and true colour;
- 4 in IShow itself: the Ogg notification offsets, the copy-everything flag, the paused event,
  and the end-of-stream quirk below.

The rest of the suite gives the same results as before the change.

What the test found in the port (not in the seam), now in the
[porting notes](porting_notes.md):

- The original marks the end of a non-looping stream only in a refill that wraps past the
  buffer's end; the refill up to the play cursor writes silence without marking it. The stream
  thread stops a marked stream at its next count of eleven notifications, so the original's music
  stops later than the port's did, which marked the end in both refills. The quirk is kept now.
- `Close` returns whatever freeing the Ogg decoder left in a register (1) when there was one; the
  port returns 0.

Two things the test has to take care of, both outside the players' logic:

- `Close` ends the stream thread with `TerminateThread`, as the original does. A thread ended
  while it is still starting up (its DLL attach calls, its first message wait) leaves a lock
  taken, and every later thread hangs. The test waits for each new thread to block in its wait.
  The game never closes a stream that soon.
- A seek while paused pauses the graph again, and its "paused" event arrives some time later.
  The test waits for it before running the graph, so that both sides see their events in the
  same order.

## Not covered

PlayModal (it reads the keyboard), Draw (IGraph's render states), the external stream API
(nothing in LS3DF installs one), Bink's own sound, and graphs that open a video window.
