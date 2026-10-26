// ISND_source: a PCM .wav file in a static sound buffer (0x1007ccf0 - 0x1007d460).
#include "isnd/isnd.h"

#include <cstdio>
#include <cstring>

#include "core/debug.h"
#include "core/fpu.h"
#include "core/i3d_result.h"
#include "external/rw_data.h"

using namespace ls3d;

namespace {

// DS3DALG_NO_VIRTUALIZATION {c241333f-1c1b-11d2-94f5-00c04fc28aca} (0x100a1bb4).
constexpr GUID kNoVirtualization = {0xc241333f, 0x1c1b, 0x11d2, {0x94, 0xf5, 0x00, 0xc0, 0x4f, 0xc2, 0x8a, 0xca}};

#pragma pack(push, 1)
struct S_riffHeader {
    char riff[4];
    uint32_t size;
    char wave[4];
};
struct S_chunkHeader {
    char id[4];
    uint32_t size;
};
// The "fmt " chunk as the original reads it: header and the PCMWAVEFORMAT fields.
struct S_formatChunk {
    S_chunkHeader header;
    uint16_t formatTag;
    uint16_t channels;
    uint32_t samplesPerSec;
    uint32_t avgBytesPerSec;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)
static_assert(sizeof(S_formatChunk) == 0x18, "fmt chunk layout");

}  // namespace

ISND_source::ISND_source()
    : refCount(1), flags(0), fileName(""), lockData(nullptr), lockData2(nullptr), lockSize(0), lockSize2(0),
      channels(0), bytesPerSample(0), frequency(0), dataSize(0), buffer(nullptr), bytesPerMs(0.0f), format()
{
    // (The original leaves flags, the lock fields and the format uninitialised.)
}

ISND_source::~ISND_source()
{
    if (buffer) {
        audio::g_pAudio->Release(buffer);
        buffer = nullptr;
    }
    channels = 0;
    bytesPerSample = 0;
    frequency = 0;
    dataSize = 0;
    bytesPerMs = 0.0f;
    fileName = "";
    if (g_sndDriver)
        g_sndDriver->sources.RemoveSwap(this);
}

int ISND_source::Release()
{
    const int refs = --refCount;
    if (refs < 0) {
        dbgPrintf("!! RefCount<0  this:0x%x  ISND_source", this);
        return 0;
    }
    if (refs == 0) {
        delete this;
        return 0;
    }
    return refs;
}

HRESULT ISND_source::Open(const char *name, uint32_t openFlags)
{
    Close();
    if (!g_sndDriver->IsInitialized())
        return I3DERR_NOTINITIALIZED;
    flags = openFlags;
    const int file = dtaOpen(name, false);
    if (file == -1) {
        dbgPrintf("ERROR!! IS_source::Open() - File not found ! %s", name);
        return I3DERR_NOFILE;
    }

    // RIFF header, then the "fmt " chunk assumed to come first.
    S_riffHeader riff;
    S_formatChunk fmt;
    dtaRead(file, &riff, sizeof riff);
    dtaRead(file, &fmt, sizeof fmt);
    if (fmt.header.size > 16)
        dtaSeek(file, static_cast<int>(fmt.header.size - 16), SEEK_CUR);
    format.wFormatTag = fmt.formatTag;
    format.nChannels = fmt.channels;
    format.nSamplesPerSec = fmt.samplesPerSec;
    format.nAvgBytesPerSec = fmt.avgBytesPerSec;
    format.nBlockAlign = fmt.blockAlign;
    format.wBitsPerSample = fmt.bitsPerSample;
    format.cbSize = 0;

    // Chunks up to "data".
    S_chunkHeader chunk;
    int read = dtaRead(file, &chunk, sizeof chunk);
    while (std::strncmp(chunk.id, "data", 4) != 0) {
        // (The original loops forever on a file without a "data" chunk.)
        if (read != static_cast<int>(sizeof chunk)) {
            dtaClose(file);
            return I3DERR_GENERIC;
        }
        dtaSeek(file, static_cast<int>(chunk.size), SEEK_CUR);
        read = dtaRead(file, &chunk, sizeof chunk);
    }
    channels = fmt.channels;
    bytesPerSample = fmt.bitsPerSample >> 3;
    frequency = static_cast<int>(fmt.samplesPerSec);
    dataSize = chunk.size;
    bytesPerMs = static_cast<float>(static_cast<double>(format.nAvgBytesPerSec) * 0.001f);
    if (format.nChannels > 1)
        flags &= ~ISNDF_3D;

    DWORD bufferFlags = DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | ((flags & ISNDF_3D) ? DSBCAPS_CTRL3D : 0);
    if (!(g_sndDriver->flags & ISND_INIT_SOFTWARE) && !(openFlags & ISNDF_SOFTWARE))
        bufferFlags |= DSBCAPS_LOCDEFER;
    else
        bufferFlags |= DSBCAPS_LOCSOFTWARE;
    DSBUFFERDESC desc = {};
    desc.dwSize = sizeof desc;
    desc.dwFlags = bufferFlags;
    desc.dwBufferBytes = dataSize;
    desc.lpwfxFormat = &format;
    if (bufferFlags & DSBCAPS_CTRL3D)
        desc.guid3DAlgorithm = kNoVirtualization;

    audio::Buffer *created = nullptr;
    if (FAILED(audio::g_pAudio->CreateSoundBuffer(g_sndDriver->dsound, &desc, &created))) {
        dtaClose(file);
        return I3DERR_GENERIC;
    }
    if (FAILED(audio::g_pAudio->Query(created, &buffer))) {
        audio::g_pAudio->Release(created);      // (leaked by the original)
        dtaClose(file);
        return I3DERR_GENERIC;
    }
    audio::g_pAudio->Release(created);

    void *data = nullptr;
    DWORD size = 0;
    if (FAILED(audio::g_pAudio->Lock(buffer, 0, 0, &data, &size, nullptr, nullptr, DSBLOCK_ENTIREBUFFER))) {
        dtaClose(file);
        return I3DERR_GENERIC;
    }
    if (static_cast<uint32_t>(dtaRead(file, data, static_cast<int>(dataSize))) != dataSize) {
        audio::g_pAudio->Unlock(buffer, data, size, nullptr, 0); // (left locked by the original)
        dtaClose(file);
        return I3DERR_GENERIC;
    }
    if (FAILED(audio::g_pAudio->Unlock(buffer, data, size, nullptr, 0))) {
        dtaClose(file);
        return I3DERR_GENERIC;
    }
    dtaClose(file);

    const char *baseName = name;
    for (const char *p = name; *p; ++p) {
        if (*p == '\\')
            baseName = p + 1;
    }
    fileName = baseName;
    return I3D_OK;
}

HRESULT ISND_source::Close()
{
    if (buffer) {
        audio::g_pAudio->Release(buffer);
        buffer = nullptr;
    }
    channels = 0;
    bytesPerSample = 0;
    frequency = 0;
    dataSize = 0;
    bytesPerMs = 0.0f;
    fileName = "";
    // (The original returns the address of fileName here.)
    return I3D_OK;
}

bool ISND_source::IsOpen()
{
    return dataSize != 0;
}

int ISND_source::GetLength()
{
    if (!dataSize)
        return 0;
    return Ftol(static_cast<float>(static_cast<double>(dataSize) / bytesPerMs));
}

HRESULT ISND_source::GetFormat(S_ISND_format *out)
{
    out->channels = channels;
    out->bytesPerSample = bytesPerSample;
    out->frequency = frequency;
    out->dataSize = dataSize;
    return I3D_OK;
}

HRESULT ISND_source::Lock(void **data)
{
    if (FAILED(audio::g_pAudio->Lock(buffer, 0, 0, &lockData, &lockSize, nullptr, nullptr, DSBLOCK_ENTIREBUFFER)))
        return I3DERR_GENERIC;
    *data = lockData;
    return I3D_OK;
}

HRESULT ISND_source::Unlock()
{
    return SUCCEEDED(audio::g_pAudio->Unlock(buffer, lockData, lockSize, nullptr, 0)) ? I3D_OK : I3DERR_GENERIC;
}
