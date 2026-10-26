// IShow: a streamed video or sound file (vtable 0x1009d9c0, 0x8c bytes; 0x10070d30 - 0x10073720),
// created by IGraph::CreateIShow. The extension picks the player:
//   .ogg  Ogg Vorbis (vorbisfile) decoded by a thread into a looping DirectSound buffer (music);
//   .bik  Bink (binkw32): frames are copied into textures by UpdateVideo;
//   .avi  DirectShow with the texture renderer filter (CTextureRenderer): frames into textures;
//   else  DirectShow's own rendering of the file (IGraphBuilder::RenderFile).
// A video picture is cut into tiles of at most the device's texture size, each drawn as a
// screen-space quad over the rectangle given to SetRect (Draw / PlayModal).
//
// The player code follows the DirectX 8 SDK's "DShowTextures" sample (CLSID_TextureRenderer).
#pragma once

#include <cstdint>

#include <windows.h>

#include "core/c_vector.h"
#include "dx/d3d8_types.h"

class ITexture;
class CTextureRenderer;
struct IGraphBuilder;
struct IMediaControl;
struct IMediaSeeking;
struct IBasicAudio;
struct IMediaEvent;
struct IDirectSoundNotify;
struct IDirectSoundBuffer8;
struct OggVorbis_File;
struct BINK;

// IShow::Open flags (the state bits share the word, IShow::flags).
enum : uint32_t {
    ISHOW_LOOP = 0x00000002,            // Ogg: start again at the end
    ISHOW_TRUECOLOR = 0x00000400,       // video tiles in 32 bits (ITC_TRUECOLOR; .avi: RGB32 frames)

    ISHOWF_FILLED = 0x08000000,         // Ogg: the buffer holds data from the decoder position
    ISHOWF_PAUSED = 0x10000000,
    ISHOWF_PLAYING = 0x20000000,
    ISHOWF_END = 0x40000000,            // Ogg: the decoder reached the end (silence follows)
    ISHOWF_OGG = 0x80000000,            // an Ogg Vorbis stream (its thread runs)
};

// GetState values.
enum {
    ISHOW_STATE_PLAYING = 0,            // (also: DirectShow, no end event yet)
    ISHOW_STATE_STOPPED = 1,            // stopped, or played to the end
    ISHOW_STATE_PAUSED = 2,
};

// A transformed, lit vertex of IGraph::DrawPrimitive's IGV_TRANSFORMED format.
struct S_showVertex {
    float x, y, z, rhw;
    uint32_t diffuse;
    uint32_t specular;                  // (never written)
    float u, v;
};

// A piece of the video picture in its own texture, drawn as a triangle strip.
struct S_showTile {
    ITexture *texture;                  // +0x00
    S_showVertex quad[4];               // +0x04 top left, top right, bottom left, bottom right
    int x, y;                           // +0x84 position in the picture
    int width, height;                  // +0x8C
};
static_assert(sizeof(S_showTile) == 0x94, "S_showTile layout");

// SetRect / GetRect / GetVideoSize: position and size.
struct S_showRect {
    int x, y, width, height;
};

class IShow {
public:
    IShow();                                                    // 0x10070d30
    ~IShow();                                                   // 0x10070eb0 (not virtual)

    // 0x00 0x10070fc0
    virtual int __stdcall AddRef();
    // 0x04 0x10070fd0: closes the file on every call (the last reference also frees the object).
    virtual int __stdcall Release();
    // 0x08 0x10071b70
    virtual bool __stdcall IsOpen();
    // 0x0C 0x10071630: ISHOW_* flags; the rectangle becomes the screen and the volume is applied.
    virtual HRESULT __stdcall Open(const char *fileName, uint32_t flags);
    // 0x10 0x10071b80
    virtual HRESULT __stdcall Close();
    // 0x14 0x10071c50
    virtual HRESULT __stdcall Play();
    // 0x18 0x10071d10: stops and rewinds.
    virtual HRESULT __stdcall Stop();
    // 0x1C 0x10071da0
    virtual HRESULT __stdcall Pause();
    // 0x20 0x10072040: screen rectangle of the picture (0: the whole screen).
    virtual HRESULT __stdcall SetRect(const S_showRect *rect);
    // 0x24 0x100720a0
    virtual HRESULT __stdcall GetRect(S_showRect *rect);
    // 0x28 0x100720e0: the picture size (x, y = 0).
    virtual HRESULT __stdcall GetVideoSize(S_showRect *size);
    // 0x2C 0x10072160: ISHOW_STATE_*, -1 when nothing is open.
    virtual int __stdcall GetState();
    // 0x30 0x100722a0: milliseconds.
    virtual HRESULT __stdcall SetPosition(int ms);
    // 0x34 0x10072260: Bink frame number.
    virtual HRESULT __stdcall SetFrame(uint32_t frame);
    // 0x38 0x10072400: milliseconds.
    virtual HRESULT __stdcall GetPosition(int *ms);
    // 0x3C 0x100724d0: 0 .. 1 (0 when there is no sound device).
    virtual HRESULT __stdcall SetVolume(float volume);
    // 0x40 0x100732f0
    virtual float __stdcall GetVolume();
    // 0x44 0x100725a0
    virtual int __stdcall GetTileCount();
    // 0x48 0x100725b0: the tiles (the game reads the first one's texture: "GetVideoTexture").
    virtual S_showTile *__stdcall GetTiles();
    // 0x4C 0x100725d0: Bink: decodes the next frame into the tiles when it is due (or now, with
    // `force`, without advancing); stops at the last frame.
    virtual HRESULT __stdcall UpdateVideo(bool force);
    // 0x50 0x10072650: draws the tiles; `keepStates` leaves the render states alone.
    virtual HRESULT __stdcall Draw(bool keepStates);
    // 0x54 0x10072820: plays the video full screen until it ends or a key is pressed.
    virtual HRESULT __stdcall PlayModal();
    // 0x58 0x10073300: DirectShow frames received.
    virtual int __stdcall GetFrameCount();

    // 0x5C - 0x78: a stream of the sound driver's external player (ISND_driver::externalApi,
    // which nothing in LS3DF installs).
    virtual HRESULT __stdcall OpenExternal(int stream, int unused);     // 0x5C 0x10073400
    virtual HRESULT __stdcall CloseExternal();                          // 0x60 0x10073500
    virtual HRESULT __stdcall PlayExternal(bool loop);                  // 0x64 0x100735b0
    virtual bool __stdcall IsExternalActive();                          // 0x68 0x10073310
    virtual HRESULT __stdcall StopExternal();                           // 0x6C 0x10073610
    virtual HRESULT __stdcall PauseExternal();                          // 0x70 0x10073640
    virtual HRESULT __stdcall SetExternal74(int value);                 // 0x74 0x10073670
    virtual HRESULT __stdcall SetExternal78(int value);                 // 0x78 0x100736a0

    // Places the tiles' quads in the screen rectangle (also after the renderer knows the size).
    void LayoutTiles();                                         // 0x10071e20

    IGraphBuilder *graph;                                       // +0x04 kept across files
    IMediaControl *control;                                     // +0x08
    IMediaSeeking *seeking;                                     // +0x0C
    void *unknown10;                                            // +0x10 never set
    IBasicAudio *audio;                                         // +0x14
    IMediaEvent *events;                                        // +0x18
    BINK *bink;                                                 // +0x1C
    bool open;                                                  // +0x20
    int refCount;                                               // +0x24
    uint32_t flags;                                             // +0x28 ISHOW_* | ISHOWF_*
    S_showRect rect;                                            // +0x2C
    float volume;                                               // +0x3C
    ls3d::C_vector<S_showTile> tiles;                           // +0x40 (grows by 1)
    CTextureRenderer *renderer;                                 // +0x50 (owned by the graph)
    int frameCount;                                             // +0x54
    int extStream;                                              // +0x58 -1: none
    bool extOpen;                                               // +0x5C
    bool extPaused;                                             // +0x5D
    volatile bool extFinished;                                  // +0x5E set by the player's callback
    DWORD threadId;                                             // +0x60
    HANDLE thread;                                              // +0x64
    HANDLE event;                                               // +0x68 buffer position notifications
    IDirectSoundNotify *notify;                                 // +0x6C
    IDirectSoundBuffer8 *buffer;                                // +0x70
    int notifyCount;                                            // +0x74
    int writePos;                                               // +0x78 next byte to fill
    int bufferSize;                                             // +0x7C
    volatile bool filling;                                      // +0x80
    OggVorbis_File *vorbis;                                     // +0x84
    int file;                                                   // +0x88 rw_data handle, -1: none

private:
    void FillBuffer(bool whole);                                // 0x10071020
    void CopyFrameToTiles(bool all);                            // 0x10071330
    HRESULT CreateTextureRenderer(const wchar_t *fileName);     // 0x100729f0
    static DWORD WINAPI StreamThread(void *show);               // 0x10071a70
};
static_assert(sizeof(IShow) == 0x8c, "IShow layout");

namespace ls3d::ig {
// Creates the tiles of a width x height picture; returns the first tile's texture format
// (0x10071440).
D3DFORMAT CreateShowTiles(int width, int height, IShow *show);
}
