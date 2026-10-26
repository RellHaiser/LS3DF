// IShow (0x10070d30 - 0x10073720): Ogg / Bink / DirectShow players, see ishow.h.
#include "igraph/ishow.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/debug.h"
#include "core/fpu.h"
#include "core/i3d_result.h"
#include "external/rw_data.h"
#include "external/vorbisfile.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"
#include "isnd/isnd.h"
#include "platform/platform.h"
#include "video/video.h"

using namespace ls3d;
using namespace ls3d::ig;

namespace {

// The players of the external stream API (0x100d25e0), for its end callback.
C_vector<IShow *> g_externalShows(0x20);

const DWORD kStreamBufferSize = 0x84000;    // 11 notification blocks of 0xC000 bytes
const DWORD kNotifyBlock = 0xc000;
const int kNotifyCount = 11;
const int kReadSize = 0x1000;

// vorbisfile's file callbacks on an rw_data handle (0x10071af0 - 0x10071b50).
size_t __cdecl OggRead(void *ptr, size_t size, size_t count, void *source)
{
    return static_cast<size_t>(dtaRead(*static_cast<int *>(source), ptr, static_cast<int>(size * count)));
}

int __cdecl OggSeek(void *source, ogg_int64_t offset, int whence)
{
    return dtaSeek(*static_cast<int *>(source), static_cast<int>(offset), whence) != -1 ? 0 : -1;
}

int __cdecl OggClose(void *source)
{
    return dtaClose(*static_cast<int *>(source)) ? 1 : 0;
}

long __cdecl OggTell(void *source)
{
    return dtaSeek(*static_cast<int *>(source), 0, SEEK_CUR);
}

// Decodes into `dst` (`left` bytes) in 4 KB reads. At the end of the stream (or an error) the rest
// is silence and the result false; with `loop` an ISHOW_LOOP stream starts again instead.
bool Decode(IShow *show, char *dst, int left, bool loop)
{
    char *const start = dst;
    int bitstream;
    while (left > 0) {
        long n = ov_read(show->vorbis, dst, left < kReadSize ? left : kReadSize, 0, 2, 1, &bitstream);
        // Fix: an error count is negative and moves the position back; the original could move
        // it before the start of the locked region (and clear memory there).
        if (n < 0 && n < start - dst)
            n = static_cast<long>(start - dst);
        dst += n;
        left -= n;
        if (n > 0)
            continue;
        if (loop && (show->flags & ISHOW_LOOP) && ov_time_seek(show->vorbis, 0.0) == 0)
            continue;
        std::memset(dst, 0, left);
        return false;
    }
    return true;
}

// The texture stage and render states of the video quads (Draw, PlayModal), through IGraph's
// state cache.
void SetVideoStates()
{
    if (g_rs.alphaOp[0] != D3DTOP_MODULATE) {
        g_rs.alphaOp[0] = D3DTOP_MODULATE;
        gfx::g_pGfx->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    }
    if (g_rs.colorOp[0] != D3DTOP_MODULATE) {
        g_rs.colorOp[0] = D3DTOP_MODULATE;
        gfx::g_pGfx->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    }
    if (g_rs.colorOp[1] != D3DTOP_DISABLE) {
        g_rs.colorOp[1] = D3DTOP_DISABLE;
        gfx::g_pGfx->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (g_rs.alpha) {
        g_rs.alpha = 0;
        gfx::g_pGfx->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        gfx::g_pGfx->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }
    if (g_rs.fog) {
        g_rs.fog = 0;
        gfx::g_pGfx->SetRenderState(D3DRS_FOGENABLE, FALSE);
    }
}

// The external player's end-of-stream callback (0x100733c0).
void __cdecl ExternalCallback(int stream, int event)
{
    if (event != 1)
        return;
    for (int i = g_externalShows.Size(); i--;) {
        if (g_externalShows[i]->extStream == stream)
            g_externalShows[i]->extFinished = true;
    }
}

// The sound device's handles released.
template <class T> void SafeReleaseAudio(T *&p)
{
    if (p) {
        audio::g_pAudio->Release(p);
        p = nullptr;
    }
}

// An .avi's frames into the player's tiles (the texture renderer of the original, which IShow's
// graph creates).
class ShowSink final : public video::FrameSink {
public:
    explicit ShowSink(IShow *show) : show(show) {}

    // 0x10072f30 (CTextureRenderer::CheckMediaType): RGB32 with ISHOW_TRUECOLOR and a true-colour
    // texture format, else the 16-bit format's layout.
    int FrameFormat() override
    {
        if ((show->flags & ISHOW_TRUECOLOR) && g_fmtTrueColor)
            return 32;
        return g_fmt16Bit == D3DFMT_R5G6B5 ? 565 : 555;
    }
    // 0x10072fe0 (CTextureRenderer::SetMediaType)
    D3DFORMAT SetSize(int width, int height) override { return CreateShowTiles(width, height, show); }
    // 0x10073070 (CTextureRenderer::DoRenderSample): the bottom-up frame cut into the tiles.
    void Frame(const uint8_t *bits, int pitch, int height, D3DFORMAT format) override
    {
        for (int i = show->tiles.Size(); i--;) {
            S_showTile &tile = show->tiles[i];
            int texturePitch;
            BYTE *dst = static_cast<BYTE *>(tile.texture->Lock(nullptr, &texturePitch, 0));
            int bytes = 0;
            switch (format) {
            case D3DFMT_A8R8G8B8:
            case D3DFMT_X8R8G8B8: bytes = 4; break;
            case D3DFMT_R5G6B5:
            case D3DFMT_X1R5G5B5:
            case D3DFMT_A1R5G5B5: bytes = 2; break;
            default:
                dbgPrintf("IShow::DoRenderSample() - ERROR! Unsuppoted destination texture format: %s !",
                          g_pIGraph->GetFormatName(format));
            }
            if (bytes) {
                const BYTE *src = bits + (height - tile.y - 1) * pitch + tile.x * bytes;
                for (int y = tile.height; y; --y) {
                    std::memcpy(dst, src, static_cast<size_t>(tile.width) * bytes);
                    dst += texturePitch;
                    src -= pitch;
                }
            }
            tile.texture->Unlock();
        }
        ++show->frameCount;
    }

private:
    IShow *show;
};

}  // namespace

// 0x10070d30
IShow::IShow()
    : graph(nullptr), unused08(nullptr), unused0C(nullptr), unknown10(nullptr), unused14(nullptr), unused18(nullptr),
      movie(nullptr), open(false), refCount(1), flags(0), rect{0, 0, 0, 0}, volume(1.0f), tiles(1), unused50(nullptr),
      frameCount(0), extStream(-1), extOpen(false), extPaused(false), extFinished(false), threadId(0),
      thread(nullptr), event(nullptr), notify(nullptr), buffer(nullptr), notifyCount(0), writePos(0), bufferSize(0),
      filling(false), vorbis(nullptr), file(-1)
{
    g_shows.PushBack(this);
    const int width = g_pIGraph->ScrnX();
    const int height = g_pIGraph->ScrnY();
    rect = {0, 0, width, height};
    LayoutTiles();
    frameCount = 0;
}

// 0x10070eb0
IShow::~IShow()
{
    g_shows.RemoveSwap(this);
    Close();
    CloseExternal();
    if (graph) {
        video::g_pVideo->ReleaseGraph(graph);
        graph = nullptr;
    }
    tiles.Free();
}

// 0x10070fc0
int __stdcall IShow::AddRef()
{
    return ++refCount;
}

// 0x10070fd0
int __stdcall IShow::Release()
{
    Close();
    if (--refCount < 0) {
        dbgPrintf("!! m_RefCount  IShow this:0x%x", this);
        return 0;
    }
    if (refCount == 0) {
        delete this;
        return 0;
    }
    return refCount;
}

// 0x10071b70
bool __stdcall IShow::IsOpen()
{
    return open;
}

// 0x10071020: refills the streaming buffer, all of it or from the last filled byte up to the play
// cursor (the thread, on each notification).
void IShow::FillBuffer(bool whole)
{
    while (filling && !whole)
        platform::Pause(0);
    filling = true;
    void *data;
    DWORD size;
    if (whole) {
        if (audio::g_pAudio->Lock(buffer, 0, 0, &data, &size, nullptr, nullptr, DSBLOCK_ENTIREBUFFER) >= 0) {
            if (!Decode(this, static_cast<char *>(data), static_cast<int>(size), false))
                flags |= ISHOWF_END;
            audio::g_pAudio->Unlock(buffer, data, size, nullptr, 0);
            writePos = 0;
        }
        filling = false;
        return;
    }
    DWORD play;
    if (audio::g_pAudio->GetCurrentPosition(buffer, &play, nullptr) < 0) {
        filling = false;
        return;
    }
    if (static_cast<int>(play) < writePos) {
        // Wrapped: up to the end of the buffer first.
        if (audio::g_pAudio->Lock(buffer, writePos, bufferSize - writePos, &data, &size, nullptr, nullptr, 0) < 0) {
            filling = false;
            return;
        }
        writePos = 0;
        if (flags & ISHOWF_END)
            std::memset(data, 0, size);
        else if (!Decode(this, static_cast<char *>(data), static_cast<int>(size), true))
            flags |= ISHOWF_END;
        audio::g_pAudio->Unlock(buffer, data, size, nullptr, 0);
    }
    if (static_cast<int>(play) > writePos) {
        if (audio::g_pAudio->Lock(buffer, writePos, play - writePos, &data, &size, nullptr, nullptr, 0) < 0) {
            filling = false;
            return;
        }
        writePos = static_cast<int>(play);
        // Quirk: an end of the stream here is not marked (silence follows, and the next refill
        // decodes again); only a refill that wraps past the buffer's end marks it.
        if (flags & ISHOWF_END)
            std::memset(data, 0, size);
        else
            Decode(this, static_cast<char *>(data), static_cast<int>(size), true);
        audio::g_pAudio->Unlock(buffer, data, size, nullptr, 0);
    }
    filling = false;
}

// 0x10071a70: refills the buffer on its notifications; stops the buffer once a whole buffer
// (11 notifications) has played after the end of the stream. Runs until Close terminates it.
DWORD WINAPI IShow::StreamThread(void *param)
{
    IShow *show = static_cast<IShow *>(param);
    for (;;) {
        platform::Pause(0);
        if (!platform::WaitEvent(show->event))
            continue;
        if (!(show->flags & ISHOWF_PLAYING))
            continue;
        if (++show->notifyCount > 10) {
            if (show->flags & ISHOWF_END) {
                audio::g_pAudio->Stop(show->buffer);
                audio::g_pAudio->SetCurrentPosition(show->buffer, 0);
                show->flags &= ~(ISHOWF_END | ISHOWF_PLAYING | ISHOWF_FILLED);
            }
            show->notifyCount = 0;
        }
        show->FillBuffer(false);
    }
}

// 0x10071330: copies the movie's current frame into the tiles.
void IShow::CopyFrameToTiles(bool all)
{
    const uint32_t copyAll = all ? video::MOVIE_COPY_ALL : 0;
    for (int i = tiles.Size(); i--;) {
        S_showTile &tile = tiles[i];
        int pitch;
        void *bits = tile.texture->Lock(nullptr, &pitch, 0);
        uint32_t surface = 0;
        switch (tile.texture->format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8: surface = video::MOVIE_RGB32; break;
        case D3DFMT_R5G6B5: surface = video::MOVIE_RGB565; break;
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5: surface = video::MOVIE_RGB555; break;
        default:
            dbgPrintf("CopyFrameToAllTextures() - ERROR! Unsupported destination texture format: %s !",
                      g_pIGraph->GetFormatName(tile.texture->format));
        }
        video::g_pVideo->CopyFrame(movie, bits, pitch, tile.height, tile.x, tile.y, tile.width, tile.height,
                                   copyAll | surface);
        tile.texture->Unlock();
    }
}

// 0x10071440
D3DFORMAT ls3d::ig::CreateShowTiles(int width, int height, IShow *show)
{
    int y = 0;
    for (int h = height; h > 0;) {
        const int th = h > g_maxTextureHeight ? g_maxTextureHeight : h;
        h -= th;
        int x = 0;
        for (int w = width; w > 0;) {
            const int tw = w > g_maxTextureWidth ? g_maxTextureWidth : w;
            w -= tw;
            S_showTile tile = {};       // the quad is set by LayoutTiles
            tile.x = x;
            tile.y = y;
            tile.width = tw;
            tile.height = th;
            ITexture *texture = g_pIGraph->CreateITexture();
            tile.texture = texture;
            texture->Create(nullptr, nullptr, (show->flags & ISHOW_TRUECOLOR) | ITC_EMPTY, tw, th, 0);
            switch (texture->format) {
            case D3DFMT_A8R8G8B8:
            case D3DFMT_X8R8G8B8: texture->Fill(nullptr, 0xff000000, 0); break;
            case D3DFMT_R5G6B5: texture->Fill(nullptr, 0, 0); break;
            case D3DFMT_X1R5G5B5:
            case D3DFMT_A1R5G5B5: texture->Fill(nullptr, 0x8000, 0); break;
            default: break;
            }
            show->tiles.PushBack(tile);
            x += tw;
        }
        y += th;
    }
    // Fix: the original read the first tile of an empty list (a zero-sized picture).
    if (show->tiles.Empty())
        return D3DFMT_UNKNOWN;
    return show->tiles[0].texture->format;
}

// 0x10071630
HRESULT __stdcall IShow::Open(const char *fileName, uint32_t openFlags)
{
    Close();
    flags = openFlags;
    if (std::strstr(fileName, ".ogg") || std::strstr(fileName, ".OGG")) {
        if (!g_sndDriver->IsInitialized())
            return I3DERR_NOTINITIALIZED;
        file = dtaOpen(fileName, false);
        if (file < 0)
            return I3DERR_NOFILE;
        vorbis = new OggVorbis_File;
        const ov_callbacks callbacks = {OggRead, OggSeek, OggClose, OggTell};
        if (ov_open_callbacks(&file, vorbis, nullptr, 0, callbacks) < 0) {
            // Fix: vorbisfile leaves the file open on failure; the original kept the handle.
            dtaClose(file);
            file = -1;
            return I3DERR_FILECORRUPTED;
        }
        const vorbis_info *info = ov_info(vorbis, -1);
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(info->channels);
        format.nSamplesPerSec = static_cast<DWORD>(info->rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * 16 / 8);
        format.nAvgBytesPerSec = format.nBlockAlign * format.nSamplesPerSec;
        bufferSize = kStreamBufferSize;
        DSBUFFERDESC desc = {};
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLVOLUME;
        desc.dwBufferBytes = kStreamBufferSize;
        desc.lpwfxFormat = &format;
        audio::Buffer *plain;
        if (audio::g_pAudio->CreateSoundBuffer(g_sndDriver->dsound, &desc, &plain) < 0)
            return I3DERR_GENERIC;
        const HRESULT hr = audio::g_pAudio->Query(plain, &buffer);
        audio::g_pAudio->Release(plain); // (the original leaked it when the query failed)
        if (hr < 0)
            return I3DERR_GENERIC;
        if (audio::g_pAudio->Query(buffer, &notify) < 0)
            return I3DERR_GENERIC;
        event = platform::NewEvent();
        DSBPOSITIONNOTIFY positions[kNotifyCount];
        int n = 0;
        for (DWORD offset = kNotifyBlock - 1; offset < kStreamBufferSize + kNotifyBlock - 1; offset += kNotifyBlock) {
            positions[n].dwOffset = offset;
            positions[n].hEventNotify = platform::NativeEvent(event);
            ++n;
        }
        if (audio::g_pAudio->SetNotificationPositions(notify, kNotifyCount, positions) < 0)
            return I3DERR_GENERIC;
        filling = false;
        FillBuffer(true);
        flags |= ISHOWF_FILLED;
        audio::g_pAudio->SetCurrentPosition(buffer, 0);
        notifyCount = 0;
        writePos = 0;
        thread = platform::StartThread(StreamThread, this);
        flags |= ISHOWF_OGG;
    } else if (std::strstr(fileName, ".bik") || std::strstr(fileName, ".BIK")) {
        if (g_sndDriver->IsInitialized())
            video::g_pVideo->UseSound(g_sndDriver->dsound);
        movie = video::g_pVideo->OpenMovie(fileName);
        if (!movie) {
            dbgPrintf("ERROR! IShow::Open() - %s", fileName);
            return I3DERR_NOFILE;
        }
        const video::S_movieInfo info = video::g_pVideo->Info(movie);
        CreateShowTiles(static_cast<int>(info.width), static_cast<int>(info.height), this);
    } else {
        if (!graph) {
            graph = video::g_pVideo->CreateGraph();
            if (!graph)
                return I3DERR_GENERIC;
        }
        HRESULT hr;
        if (std::strstr(fileName, ".avi") || std::strstr(fileName, ".AVI")) {
            // (0x100729f0, m_CreateCTextureRenderer: the frames into the tiles, then their layout)
            hr = video::g_pVideo->RenderToSink(graph, fileName, new ShowSink(this));
            if (hr >= 0)
                LayoutTiles();
        } else {
            hr = video::g_pVideo->RenderFile(graph, fileName);
        }
        if (hr < 0) {
            dbgPrintf("ERROR! IShow::Open() - %s", fileName);
            return I3DERR_NOFILE;
        }
    }
    open = true;
    SetRect(nullptr);
    SetVolume(volume);
    return 0;
}

// 0x10071b80
HRESULT __stdcall IShow::Close()
{
    Stop();
    for (int i = tiles.Size(); i--;)
        tiles[i].texture->Release();
    tiles.Free();
    if (movie) {
        video::g_pVideo->CloseMovie(movie);
        movie = nullptr;
    }
    if (event) {
        platform::Terminate(thread, 0);
        platform::Release(thread);      // (the original never closed the thread handle)
        thread = nullptr;
        threadId = 0;
        platform::DeleteEvent(event);
        event = nullptr;
    }
    if (file >= 0) {
        ov_clear(vorbis);               // closes the file
        file = -1;
    }
    if (vorbis) {
        delete vorbis;
        vorbis = nullptr;
    }
    // Fix: the original kept (leaked) the streaming buffer and its notification interface; the
    // next Ogg file creates new ones.
    SafeReleaseAudio(notify);
    SafeReleaseAudio(buffer);
    frameCount = 0;
    open = false;
    return 0;
}

// 0x10071c50
HRESULT __stdcall IShow::Play()
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (movie) {
        if (flags & ISHOWF_PAUSED) {
            video::g_pVideo->Pause(movie, false);
            flags &= ~ISHOWF_PAUSED;
        }
        flags |= ISHOWF_PLAYING;
        return 0;
    }
    if (flags & ISHOWF_OGG) {
        if (!(flags & ISHOWF_PLAYING)) {
            if (!(flags & ISHOWF_FILLED)) {
                if (flags & ISHOWF_PAUSED) {
                    // Pause stopped the buffer: continue from where the decoder is.
                    filling = false;
                    FillBuffer(true);
                    flags |= ISHOWF_FILLED;
                } else {
                    SetPosition(0);
                }
            }
            audio::g_pAudio->Play(buffer, 0, 0, DSBPLAY_LOOPING);
            flags = (flags & ~(ISHOWF_END | ISHOWF_PAUSED)) | ISHOWF_PLAYING;
        }
        return 0;
    }
    video::g_pVideo->Run(graph);
    return 0;
}

// 0x10071d10
HRESULT __stdcall IShow::Stop()
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (movie) {
        flags &= ~(ISHOWF_PLAYING | ISHOWF_PAUSED);
        SetPosition(0);
        return 0;
    }
    if (flags & ISHOWF_OGG) {
        audio::g_pAudio->Stop(buffer);
        audio::g_pAudio->SetCurrentPosition(buffer, 0);
        flags &= ~(ISHOWF_END | ISHOWF_PLAYING | ISHOWF_PAUSED | ISHOWF_FILLED);
        notifyCount = 0;
        writePos = 0;
        return 0;
    }
    video::g_pVideo->Stop(graph);
    SetPosition(0);
    frameCount = 0;
    return 0;
}

// 0x10071da0
HRESULT __stdcall IShow::Pause()
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (movie) {
        if (flags & ISHOWF_PLAYING) {
            video::g_pVideo->Pause(movie, true);
            flags = (flags & ~ISHOWF_PLAYING) | ISHOWF_PAUSED;
        }
    } else if (flags & ISHOWF_OGG) {
        // A stop (the buffer rewinds; Play refills it from the decoder position).
        if (flags & ISHOWF_PLAYING) {
            Stop();
            flags |= ISHOWF_PAUSED;
        }
    } else {
        video::g_pVideo->Pause(graph);
    }
    return 0;
}

// 0x10071e20
void IShow::LayoutTiles()
{
    float scaleX, scaleY;
    int videoWidth, videoHeight;
    if (movie) {
        const video::S_movieInfo info = video::g_pVideo->Info(movie);
        scaleX = static_cast<float>(rect.width) / static_cast<float>(static_cast<int>(info.width));
        scaleY = static_cast<float>(rect.height) / static_cast<float>(static_cast<int>(info.height));
    } else if (video::g_pVideo->VideoSize(graph, &videoWidth, &videoHeight)) {
        scaleX = static_cast<float>(rect.width) / static_cast<float>(videoWidth);
        scaleY = static_cast<float>(rect.height) / static_cast<float>(videoHeight);
    } else {
        return;
    }
    const float left = static_cast<float>(rect.x), top = static_cast<float>(rect.y);
    for (int i = tiles.Size(); i--;) {
        S_showTile &t = tiles[i];
        S_showVertex *q = t.quad;
        q[0].diffuse = 0xffffffff;
        q[0].x = static_cast<float>(t.x) * scaleX + left;
        q[0].y = static_cast<float>(t.y) * scaleY + top;
        q[0].z = 0.0f;
        q[0].rhw = 1.0f;
        q[0].u = 0.0f;
        q[0].v = 0.0f;
        q[1].diffuse = 0xffffffff;
        q[1].x = static_cast<float>(t.width + t.x) * scaleX + left;
        q[1].y = q[0].y;
        q[1].z = 0.0f;
        q[1].rhw = 1.0f;
        q[1].u = static_cast<float>(t.width) / static_cast<float>(t.texture->width);
        q[1].v = 0.0f;
        q[2].diffuse = 0xffffffff;
        q[2].x = q[0].x;
        q[2].y = static_cast<float>(t.height + t.y) * scaleY + top;
        q[2].z = 0.0f;
        q[2].rhw = 1.0f;
        q[2].u = 0.0f;
        q[2].v = static_cast<float>(t.height) / static_cast<float>(t.texture->height);
        q[3].diffuse = 0xffffffff;
        q[3].x = q[1].x;
        q[3].y = q[2].y;
        q[3].z = 0.0f;
        q[3].rhw = 1.0f;
        q[3].u = q[1].u;
        q[3].v = q[2].v;
    }
}

// 0x10072040
HRESULT __stdcall IShow::SetRect(const S_showRect *r)
{
    if (r)
        rect = *r;
    else
        rect = {0, 0, g_pIGraph->ScrnX(), g_pIGraph->ScrnY()};
    LayoutTiles();
    return 0;
}

// 0x100720a0
HRESULT __stdcall IShow::GetRect(S_showRect *r)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    *r = rect;
    return 0;
}

// 0x100720e0
HRESULT __stdcall IShow::GetVideoSize(S_showRect *size)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    *size = {0, 0, 0, 0};
    if (movie) {
        const video::S_movieInfo info = video::g_pVideo->Info(movie);
        size->width = static_cast<int>(info.width);
        size->height = static_cast<int>(info.height);
        return 0;
    }
    if (!video::g_pVideo->VideoSize(graph, &size->width, &size->height))
        return I3DERR_NOTINITIALIZED;
    return 0;
}

// 0x10072160
int __stdcall IShow::GetState()
{
    if (!open)
        return -1;
    if (movie || (flags & ISHOWF_OGG)) {
        if (flags & ISHOWF_PLAYING)
            return ISHOW_STATE_PLAYING;
        return (flags & ISHOWF_PAUSED) ? ISHOW_STATE_PAUSED : ISHOW_STATE_STOPPED;
    }
    long code;
    while (video::g_pVideo->NextEvent(graph, &code)) {
        if (code > 0) {
            if (code <= video::GRAPH_EVENT_USERABORT) {     // complete, user abort
                Stop();
                return ISHOW_STATE_STOPPED;
            }
            if (code == video::GRAPH_EVENT_PAUSED)
                return ISHOW_STATE_PAUSED;
        }
    }
    return ISHOW_STATE_PLAYING;
}

// 0x100722a0
HRESULT __stdcall IShow::SetPosition(int ms)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (movie) {
        // Quirk: frame = rateDiv * ms / (rate * 0.001), not the inverse of GetPosition's
        // ms = rateDiv * frame * 1000 / rate (exact for 0 only).
        const video::S_movieInfo info = video::g_pVideo->Info(movie);
        const double num = static_cast<double>(static_cast<uint32_t>(info.rateDivisor * static_cast<uint32_t>(ms)));
        const float den = static_cast<float>(static_cast<double>(info.rate) * 0.001f);
        video::g_pVideo->Goto(movie, static_cast<uint32_t>(Ftol(static_cast<float>(num / den))));
        return 0;
    }
    if (flags & ISHOWF_OGG) {
        if (flags & ISHOWF_PLAYING)
            audio::g_pAudio->Stop(buffer);
        const float seconds = static_cast<float>(static_cast<double>(static_cast<uint32_t>(ms)) * 0.001f);
        if (ov_time_seek(vorbis, seconds) != 0)
            return I3DERR_GENERIC;
        flags &= ~ISHOWF_END;
        filling = false;
        FillBuffer(true);
        flags |= ISHOWF_FILLED;
        audio::g_pAudio->SetCurrentPosition(buffer, 0);
        if (flags & ISHOWF_PLAYING) {
            audio::g_pAudio->Play(buffer, 0, 0, DSBPLAY_LOOPING);
            return 0;
        }
        flags |= ISHOWF_PAUSED;
        return 0;
    }
    // (100 ns units, multiplied in 32 bits as in the original)
    video::g_pVideo->SetPosition(graph, static_cast<uint32_t>(ms * 10000));
    return 0;
}

// 0x10072260
HRESULT __stdcall IShow::SetFrame(uint32_t frame)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (movie) {
        const uint32_t frames = video::g_pVideo->Info(movie).frames;
        if (frame > frames)
            frame = frames;
        video::g_pVideo->Goto(movie, frame);
    }
    return 0;
}

// 0x10072400
HRESULT __stdcall IShow::GetPosition(int *ms)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (movie) {
        const video::S_movieInfo info = video::g_pVideo->Info(movie);
        *ms = static_cast<int>(info.rateDivisor * info.frame * 1000u / info.rate);
        return 0;
    }
    if (flags & ISHOWF_OGG) {
        DWORD play;
        if (audio::g_pAudio->GetCurrentPosition(buffer, &play, nullptr) < 0)
            return I3DERR_GENERIC;
        *ms = Ftol(static_cast<float>(ov_time_tell(vorbis) * 1000.0));
        return 0;
    }
    *ms = static_cast<int>(video::g_pVideo->GetPosition(graph) / 10000);
    return 0;
}

// 0x100724d0
HRESULT __stdcall IShow::SetVolume(float v)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (!g_sndDriver->IsInitialized())
        v = 0.0f;
    volume = v;
    if (movie) {
        video::g_pVideo->SetVolume(movie, Ftol(v * 32768.0f));
        return 0;
    }
    // Hundredths of a decibel: 866 ln(v), at least -10000.
    int db;
    if (v < 1e-8f) {
        db = DSBVOLUME_MIN;
    } else if (!(v >= 1.0f)) {
        db = Ftol(static_cast<float>(std::log(static_cast<double>(v)) * 866.0));
        if (db < DSBVOLUME_MIN)
            db = DSBVOLUME_MIN;
    } else {
        db = 0;
    }
    if (flags & ISHOWF_OGG)
        audio::g_pAudio->SetVolume(buffer, db);
    else
        video::g_pVideo->SetVolume(graph, db);
    return 0;
}

// 0x100732f0
float __stdcall IShow::GetVolume()
{
    return volume;
}

// 0x100725a0
int __stdcall IShow::GetTileCount()
{
    return tiles.Size();
}

// 0x100725b0
S_showTile *__stdcall IShow::GetTiles()
{
    platform::Pause(0);
    return tiles.Data();
}

// 0x100725d0
HRESULT __stdcall IShow::UpdateVideo(bool force)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    if (((flags & ISHOWF_PLAYING) || force) && movie) {
        if (!video::g_pVideo->Wait(movie) || force) {
            video::g_pVideo->DecodeFrame(movie);
            CopyFrameToTiles(force);
            if (!force) {
                const video::S_movieInfo info = video::g_pVideo->Info(movie);
                if (info.frame == info.frames) {
                    Stop();
                    return 0;
                }
                video::g_pVideo->NextFrame(movie);
            }
        }
    }
    return 0;
}

// 0x10072650
HRESULT __stdcall IShow::Draw(bool keepStates)
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    UpdateVideo(false);
    if (!keepStates) {
        SetVideoStates();
        gfx::g_pGfx->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        gfx::g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
        gfx::g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    for (int i = tiles.Size(); i--;) {
        g_pIGraph->SetTexture(tiles[i].texture, 0);
        g_pIGraph->DrawPrimitive(IGP_TRIANGLESTRIP, 2, tiles[i].quad, IGV_TRANSFORMED);
    }
    if (!keepStates) {
        // (IGraph's cached address modes are not updated)
        gfx::g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        gfx::g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
        gfx::g_pGfx->SetRenderState(D3DRS_ZENABLE, g_zBufferType);
    }
    return 0;
}

// 0x10072820
HRESULT __stdcall IShow::PlayModal()
{
    if (!open)
        return I3DERR_NOTINITIALIZED;
    Play();
    SetVideoStates();
    int drawn = frameCount - 1;
    if (GetState() == ISHOW_STATE_STOPPED)
        return 0;
    for (;;) {
        g_pIGraph->ProcessWinMessages();
        g_pIGraph->ReadKeyboard();
        if (g_pIGraph->GetKey()) {
            Stop();
            return 0;
        }
        // A movie draws every pass (Draw decodes when a frame is due); a graph when the renderer
        // delivered a new frame.
        if (movie || drawn != frameCount) {
            const int frame = frameCount;
            g_pIGraph->Clear(0, 1.0f, 3);
            g_pIGraph->BeginScene();
            Draw(false);
            g_pIGraph->EndScene();
            g_pIGraph->Flip();
            if (!movie)
                drawn = frame;
        } else {
            platform::Pause(0);
        }
        if (GetState() == ISHOW_STATE_STOPPED)
            return 0;
    }
}

// 0x10073300
int __stdcall IShow::GetFrameCount()
{
    return frameCount;
}

// 0x10073400
HRESULT __stdcall IShow::OpenExternal(int stream, int)
{
    CloseExternal();
    if (g_sndDriver->externalApi->Startup(0) < 0)
        return I3DERR_UNSUPPORTED;
    extOpen = true;
    if (g_sndDriver->externalApi->OpenStream(0, &extStream, ExternalCallback, stream, 0) < 0)
        return I3DERR_NOFILE;
    for (IShow *s : g_externalShows) {
        if (s == this)
            return 0;
    }
    g_externalShows.PushBack(this);
    return 0;
}

// 0x10073500: returns the last API result (-1 when there was nothing to close).
HRESULT __stdcall IShow::CloseExternal()
{
    extPaused = false;
    extFinished = false;
    g_externalShows.RemoveSwap(this);
    HRESULT hr = extStream;
    if (extStream != -1) {
        StopExternal();
        hr = g_sndDriver->externalApi->CloseStream(extStream);
        if (hr < 0)
            return hr;
        extStream = -1;
    }
    if (extOpen) {
        hr = g_sndDriver->externalApi->Shutdown(0);
        if (hr >= 0)
            extOpen = false;
    }
    return hr;
}

// 0x100735b0
HRESULT __stdcall IShow::PlayExternal(bool loop)
{
    if (extPaused) {
        extPaused = false;
        if (g_sndDriver->externalApi->Resume(extStream) < 0)
            return I3DERR_GENERIC;
    } else if (g_sndDriver->externalApi->Play(extStream, loop) < 0) {
        return I3DERR_GENERIC;
    }
    return 0;
}

// 0x10073310
bool __stdcall IShow::IsExternalActive()
{
    return !extFinished;
}

// 0x10073610
HRESULT __stdcall IShow::StopExternal()
{
    return g_sndDriver->externalApi->Stop(extStream) >= 0 ? 0 : I3DERR_GENERIC;
}

// 0x10073640
HRESULT __stdcall IShow::PauseExternal()
{
    if (g_sndDriver->externalApi->Pause(extStream) < 0)
        return I3DERR_GENERIC;
    extPaused = true;
    return 0;
}

// 0x10073670
HRESULT __stdcall IShow::SetExternal74(int value)
{
    return g_sndDriver->externalApi->Set20(extStream, 0, value) >= 0 ? 0 : I3DERR_GENERIC;
}

// 0x100736a0
HRESULT __stdcall IShow::SetExternal78(int value)
{
    return g_sndDriver->externalApi->Set1C(extStream, 0, value) >= 0 ? 0 : I3DERR_GENERIC;
}
