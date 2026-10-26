// The video device: IShow plays its videos through video::Device (g_pVideo).
//
// The engine has two kinds of video player, and the device keeps both:
// - Movies (Bink, the game's videos): IShow steps through the frames itself, decoding the next
//   one when it is due and copying it into its tiles. The vocabulary is Bink's: frame numbers from
//   1, the frame rate as rate / divisor, and the destination formats of a frame copy. This is the
//   model of a decoder that IShow pulls frames from (FFmpeg is the planned successor).
// - Graphs (DirectShow, for .avi and other files): the player runs by itself. An .avi's frames are
//   pushed into IShow's tiles through a FrameSink; other files are shown by the player's own
//   renderers.
// Ogg Vorbis music is not here: vorbisfile is portable, and the stream plays through the sound
// device (audio/audio.h).
//
// Movies and graphs are opaque handles that only the backend looks inside. The DirectShow
// interfaces and the texture renderer filter belong to the backend.
#pragma once

#include <cstdint>

#include "dx/d3d8_types.h"

namespace ls3d::audio {
struct System;
}  // namespace ls3d::audio

namespace ls3d::video {

// Opaque handles (defined only by a backend).
struct Movie;
struct Graph;

// A movie's properties (Bink's).
struct S_movieInfo {
    uint32_t width, height;
    uint32_t frames;                    // number of frames
    uint32_t frame;                     // the current one (from 1)
    uint32_t rate, rateDivisor;         // frames per second: rate / rateDivisor
};

// CopyFrame's destination formats and flags (Bink's values).
enum : uint32_t {
    MOVIE_RGB32 = 3,
    MOVIE_RGB555 = 9,
    MOVIE_RGB565 = 10,
    MOVIE_COPY_ALL = 0x80000000,        // the whole picture, not only what changed
};

// Graph event codes NextEvent reports (DirectShow's).
enum : long {
    GRAPH_EVENT_COMPLETE = 1,
    GRAPH_EVENT_USERABORT = 2,
    GRAPH_EVENT_PAUSED = 0x0e,
};

// Where a graph puts the frames of a video it renders into textures (IShow's tiles).
class FrameSink {
public:
    virtual ~FrameSink() = default;
    // The pixel layout the frames should come in: 32 (RGB32), 565 or 555 (16-bit).
    virtual int FrameFormat() = 0;
    // The picture's size is known: the sink makes its tiles and returns their texture format
    // (D3DFMT_UNKNOWN: failure).
    virtual D3DFORMAT SetSize(int width, int height) = 0;
    // A frame: `height` bottom-up rows of `pitch` bytes, for tiles of `format`.
    virtual void Frame(const uint8_t *bits, int pitch, int height, D3DFORMAT format) = 0;
};

class Device {
public:
    virtual ~Device() = default;

    // ---- movies (Bink)
    // The movies opened from now on play their sound through this sound device API object.
    virtual void UseSound(audio::System *system) = 0;
    // Null on failure.
    virtual Movie *OpenMovie(const char *fileName) = 0;
    virtual void CloseMovie(Movie *movie) = 0;
    virtual S_movieInfo Info(Movie *movie) = 0;
    virtual void Pause(Movie *movie, bool pause) = 0;
    virtual void Goto(Movie *movie, uint32_t frame) = 0;
    // True while the next frame is not due yet.
    virtual bool Wait(Movie *movie) = 0;
    // Decodes the current frame.
    virtual void DecodeFrame(Movie *movie) = 0;
    virtual void NextFrame(Movie *movie) = 0;
    // Copies a width x height part of the decoded frame at (x, y) into `dest` (`pitch` bytes per
    // row, `destHeight` rows), in a MOVIE_* format (| MOVIE_COPY_ALL).
    virtual void CopyFrame(Movie *movie, void *dest, int pitch, uint32_t destHeight, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, uint32_t format) = 0;
    // 0 - 32768.
    virtual void SetVolume(Movie *movie, int32_t volume) = 0;

    // ---- graphs (DirectShow)
    // Null on failure.
    virtual Graph *CreateGraph() = 0;
    virtual void ReleaseGraph(Graph *graph) = 0;
    // Adds the file and the player's own renderers for it.
    virtual HRESULT RenderFile(Graph *graph, const char *fileName) = 0;
    // Adds the file with its video going to `sink` (owned by the graph from now on) and its sound to
    // the player's sound renderer.
    virtual HRESULT RenderToSink(Graph *graph, const char *fileName, FrameSink *sink) = 0;
    virtual void Run(Graph *graph) = 0;
    virtual void Stop(Graph *graph) = 0;
    virtual void Pause(Graph *graph) = 0;
    // In 100 ns units.
    virtual void SetPosition(Graph *graph, int64_t position) = 0;
    virtual int64_t GetPosition(Graph *graph) = 0;
    // Hundredths of a decibel (0: full volume).
    virtual void SetVolume(Graph *graph, long volume) = 0;
    // The graph's next event (GRAPH_EVENT_* and others); false when none waits.
    virtual bool NextEvent(Graph *graph, long *code) = 0;
    // The picture size of the last video rendered into a sink (false: none in this graph).
    virtual bool VideoSize(Graph *graph, int *width, int *height) = 0;
};

// The device in use (the Windows backend's, video/win32: Bink and DirectShow).
extern Device *g_pVideo;

}  // namespace ls3d::video
