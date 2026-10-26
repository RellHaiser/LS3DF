// I3D_sound (0x10054310 - 0x100560f0).
#include "i3d/sound.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "core/debug.h"
#include "core/fpu.h"
#include "i3d/area.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/render_state.h"
#include "i3d/sector.h"
#include "isnd/isnd.h"
#include "platform/platform.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace {

bool Same(float a, float b) { return IsTinyBits(std::fabs(a - b)); }

const char *SourceName(ISND_source *source) { return source ? source->fileName.c_str() : "???"; }

// cos of half an angle (FCOS of the 24-bit product; the result rounded once to float).
float HalfCos(float angle) { return static_cast<float>(std::cos(static_cast<double>(angle * 0.5f))); }

}  // namespace

// 0x10054310
void I3D_sound::Init()
{
    I3D_frame::Init();
    volume = 1.0f;
    lastVolume = 1.0f;
    frequency = 1.0f;
    nearRange = 1.0f;
    nearScaled = 1.0f;
    midFraction = 1.0f;
    farRange = 10.0f;
    farScaled = 10.0f;
    velocity.x = 0.0f;
    velocity.y = 0.0f;
    velocity.z = 0.0f;
    soundFlags = 0x208f;
    state = SNDST_STOPPED;
    changeBase = 0;
    stopTime = 0;
    unknown130 = 0;
    playPosition = 0;
    sound = nullptr;
    source = nullptr;
    loop = false;
    outVolume = 0.0f;
    sourceFlags = 0;
    farScaledSq = 100.0f;
    midScaled = 0.0f;
    midVolume = 0.0f;
    soundType = I3DSOUND_NONE;
    coneInner = 1.04719758f;
    coneOuter = 1.57079637f;
    velocity.w = 0.0f;                  // (the original stores an unset local)
    envSector = nullptr;
    areaFadeTime = 0;
    areaFade = -1.0f;
    areaFadeDuration = 0;
    priority = 0;
    coneInnerCos = static_cast<float>(std::cos(static_cast<double>(0.523598790f)));
    coneOuterCos = static_cast<float>(std::cos(static_cast<double>(0.785398185f)));
    g_driver->soundFrames.PushBack(this);
}

// 0x100544b0
void I3D_sound::Uninit()
{
    SetOn(false);
    // (RemoveSound takes the sound out of `sectors` while this walks it: the walk reads the stale
    // entries the removals leave behind, as the original does.)
    I3D_sector **s = sectors.Data();
    for (int n = sectors.Size(); n != 0; --n, ++s)
        (*s)->RemoveSound(this);
    sectors.Clear();
    I3D_area **a = areas.Data();
    for (int n = areas.Size(); n != 0; --n, ++a)
        (*a)->RemoveSound(this);
    areas.Clear();
    g_driver->soundFrames.RemoveSwap(this);
    if (source) {
        source->Release();
        source = nullptr;
    }
    if (sound) {
        sound->Release();
        sound = nullptr;
    }
    I3D_frame::Uninit();
}

// 0x100560f0
void I3D_sound::Load() { I3D_frame::Load(); }

// 0x100545a0
I3D_RESULT I3D_sound::Open(const char *fileName, uint32_t flags)
{
    if (!g_sndDriver->IsInitialized())
        return I3DERR_NOTINITIALIZED;
    if (!fileName)
        return I3DERR_NOFILE;
    flags |= ISNDF_3D;
    if (sound) {
        if (std::strcmp(fileName, source->fileName.c_str()) == 0)
            return I3D_OK;
        if (source)
            source->Release();
        if (sound)
            sound->Release();
        sound = nullptr;
        source = nullptr;
    }
    if (!*fileName)
        return I3D_OK;
    const char *slash = std::strrchr(fileName, '\\');
    const char *baseName = slash ? slash + 1 : fileName;
    for (int i = g_sndDriver->sources.Size(); i != 0; --i) {
        ISND_source *s = g_sndDriver->sources[i - 1];
        if (_stricmp(baseName, s->fileName.c_str()) == 0) {
            source = s;
            ++s->refCount;
            break;
        }
    }
    if (!source) {
        ISND_source *s = nullptr;
        if (g_sndDriver->CreateSource(&s) < 0)
            return I3DERR_GENERIC;
        uint32_t openFlags = flags;
        if (openFlags & 8)
            openFlags |= ISNDF_FREQUENCY;
        if (s->Open(fileName, openFlags) < 0) {
            s->Release();
            return I3DERR_GENERIC;
        }
        g_sndDriver->sources.PushBack(s);
        source = s;
    }
    sourceFlags = source->flags;
    if (g_sndDriver->Create3DSound(&sound) < 0)
        return I3DERR_GENERIC;
    if (sound->Open(source, sourceFlags) < 0)
        return I3DERR_GENERIC;
    if (sourceFlags & ISNDF_3D) {
        sound->SetDistances(1e9f, 1e9f);
        SetCone(1.04719758f, 1.57079637f);
    } else if (soundType != I3DSOUND_NONE && soundType != I3DSOUND_AMBIENT) {
        S_ISND_format format;
        source->GetFormat(&format);
        if (static_cast<uint32_t>(format.channels) > 1)
            dbgPrintf("WARNING ! I3D_sound::Open() - STEREO sound[%s] can be ambient only. Forcing to ambient type!",
                      source->fileName.c_str());
        else
            dbgPrintf("WARNING ! I3D_sound::Open() - Sound[%s], which isn't 3D, can be ambient only. Forcing to ambient type!",
                      SourceName(source));
        SetSoundType(I3DSOUND_AMBIENT);
    }
    if (frameFlags & FRMFLAGS_ON)
        SetOn(true);
    SetCurrTime(0);
    soundFlags = 0x21ff;
    return I3D_OK;
}

// 0x100548d0
bool I3D_sound::IsPlaying(bool device)
{
    if (!sound)
        return false;
    if (device)
        return sound->IsPlaying();
    if (state == SNDST_STOPPED)
        return false;
    if (soundFlags & SNDF_PLAY)
        return true;
    if (state != SNDST_OUT_OF_RANGE && state != SNDST_AUDIBLE)
        return sound->IsPlaying();
    if (loop)
        return true;
    // Out of range: still playing if the position it would have reached is before the end.
    const uint32_t elapsed = platform::Milliseconds() - stopTime;
    const uint32_t length = static_cast<uint32_t>(
        Ftol(static_cast<float>(static_cast<double>(static_cast<uint32_t>(GetLength())) / frequency)));
    return static_cast<uint32_t>(playPosition) + elapsed < length;
}

// 0x100549a0
void I3D_sound::SetSoundType(int newType)
{
    if (soundType == newType)
        return;
    if (!source || (sourceFlags & ISNDF_3D) || newType == I3DSOUND_NONE || newType == I3DSOUND_AMBIENT) {
        soundType = newType;
    } else {
        S_ISND_format format;
        source->GetFormat(&format);
        if (static_cast<uint32_t>(format.channels) < 2)
            dbgPrintf("WARNING ! I3D_sound::SetSoundType() - Sound[%s], which isn't 3D, can be ambient only. Forcing to ambient type!",
                      SourceName(source));
        else
            dbgPrintf("WARNING ! I3D_sound::SetSoundType() - STEREO sound[%s] can be ambient only. Forcing to ambient type!",
                      source->fileName.c_str());
        soundType = I3DSOUND_AMBIENT;
    }
    soundFlags |= SNDF_TYPE;
}

// 0x10054a40
void I3D_sound::SetRange(float nearRange_, float farRange_, float midFraction_, float midVolume_)
{
    if (Same(nearRange, nearRange_) && Same(farRange, farRange_) && Same(midFraction, midFraction_) &&
        Same(midVolume, midVolume_))
        return;
    nearRange = nearRange_;
    midFraction = midFraction_;
    farRange = farRange_;
    midVolume = midVolume_;
    soundFlags |= SNDF_RANGE;
}

// 0x10054b20
void I3D_sound::SetCone(float inner, float outer)
{
    if (Same(coneInner, inner) && Same(coneOuter, outer))
        return;
    coneInner = inner;
    coneOuter = outer;
    if (inner > 3.14159274f)
        coneInner = 3.14159274f;
    if (outer > 3.14159274f)
        coneOuter = 3.14159274f;
    soundFlags |= SNDF_CONE;
    coneInnerCos = HalfCos(coneInner);
    coneOuterCos = HalfCos(coneOuter);
}

// 0x10054c00
void I3D_sound::SetOutVolume(float v)
{
    if (Same(outVolume, v))
        return;
    outVolume = v;
    soundFlags |= SNDF_OUT_VOLUME;
}

// 0x10054c40
void I3D_sound::SetVolume(float v)
{
    if (Same(volume, v))
        return;
    volume = v;
    soundFlags |= SNDF_VOLUME;
}

// 0x10054c80
void I3D_sound::SetLoop(bool on)
{
    loop = on;
    if (on)
        sourceFlags |= ISNDP_LOOP;
    else
        sourceFlags &= ~ISNDP_LOOP;
    if (frameFlags & FRMFLAGS_ON)
        SetOn(true);
}

// 0x10054cd0
I3D_RESULT I3D_sound::SetCurrTime(int position)
{
    if (!sound)
        return I3D_OK;
    HRESULT hr;
    if (position == ISND_PAUSE || position == ISND_RESUME) {
        hr = sound->SetPlayPosition(position);
    } else {
        playPosition = position;
        if (state == SNDST_OUT_OF_RANGE || state == SNDST_AUDIBLE) {
            stopTime = platform::Milliseconds();
            if (state == SNDST_OUT_OF_RANGE)
                return I3D_OK;
        }
        hr = sound->SetPlayPosition(position);
    }
    return hr < 0 ? I3DERR_GENERIC : I3D_OK;
}

// 0x10054d50
void I3D_sound::SetFrequency(float ratio)
{
    if (Same(frequency, ratio))
        return;
    if (ratio >= 0.01f && ratio <= 100.0f) {
        frequency = ratio;
        soundFlags |= SNDF_FREQUENCY;
    }
}

// 0x10054e80
void I3D_sound::GetRange(float &nearRange_, float &farRange_, float &midFraction_, float &midVolume_)
{
    nearRange_ = nearRange;
    farRange_ = farRange;
    midFraction_ = midFraction;
    midVolume_ = midVolume;
}

// 0x10054ec0
void I3D_sound::GetCone(float &inner, float &outer)
{
    inner = coneInner;
    outer = coneOuter;
}

// 0x10054ee0
int I3D_sound::GetCurrTime() { return sound ? sound->GetPlayPosition() : 0; }

// 0x10054f00
int I3D_sound::GetLength() { return sound ? source->GetLength() : 0; }

// 0x10054dc0
void I3D_sound::SetVelocity(const S_vector4 &v)
{
    if (Same(velocity.x, v.x) && Same(velocity.y, v.y) && Same(velocity.z, v.z))
        return;
    velocity = v;
    soundFlags |= SNDF_VELOCITY;
}

// 0x10055200
void I3D_sound::UpdateSound() { Refresh(); }

// 0x1001bb10
void __stdcall I3D_sound::SetOn(bool on) { SetActive(on, false); }

void I3D_sound::StopOutOfRange()
{
    state = SNDST_OUT_OF_RANGE;
    soundFlags &= ~SNDF_PLAY;
    stopTime = platform::Milliseconds();
    playPosition = GetCurrTime();
    if (sound)
        sound->Stop();
}

// 0x10054f30
void I3D_sound::SetActive(bool on, bool update)
{
    I3D_frame::SetOn(on);
    if (!sound)
        return;
    if (soundType == I3DSOUND_NONE) {
        sound->Stop();
        state = SNDST_STOPPED;
        return;
    }
    if (!on) {
        sound->Stop();
        soundFlags &= ~SNDF_PLAY;
        state = SNDST_STOPPED;
        return;
    }
    const uint8_t old = state;
    if (old == SNDST_STOPPED)
        playPosition = 0;
    if (!g_renderCamera && (soundType != I3DSOUND_AMBIENT || sectors.Size() > 0)) {
        state = SNDST_NO_CAMERA;
        return;
    }
    if (sectors.Size() > 0) {
        bool found = false;
        for (int i = 0; i < sectors.Size(); ++i) {
            if (g_renderCamera->sector == sectors[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (old != SNDST_OUT_OF_RANGE)
                StopOutOfRange();
            return;
        }
        soundFlags |= SNDF_IN_CAMERA_SECTOR;
    }
    if (areas.Size() > 0) {
        bool found = false;
        for (int i = 0; i < areas.Size() && !found; ++i) {
            for (int j = 0; j < g_activeAreas.Size(); ++j) {
                if (g_activeAreas[j] == areas[i]) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            if (old != SNDST_OUT_OF_RANGE)
                StopOutOfRange();
            return;
        }
        soundFlags = (soundFlags & ~SNDF_AREA_LEAVING) | SNDF_AREA_ACTIVE;
    }
    if (old == SNDST_OUT_OF_RANGE || old == SNDST_AUDIBLE) {
        // Resume where it would be by now (looping: wrapped).
        const uint32_t elapsed = platform::Milliseconds() - stopTime;
        const uint32_t length = static_cast<uint32_t>(
            Ftol(static_cast<float>(static_cast<double>(static_cast<uint32_t>(GetLength())) / frequency)));
        if (!loop && static_cast<uint32_t>(playPosition) + elapsed >= length) {
            state = SNDST_STOPPED;
            return;
        }
        int pos = 0;
        if (length != 0) {
            const int32_t wrapped = static_cast<int32_t>((static_cast<uint32_t>(playPosition) + elapsed) % length);
            pos = Ftol(static_cast<float>(wrapped) * frequency);
        }
        SetCurrTime(pos);
        playPosition = pos;
    }
    soundFlags |= SNDF_PLAY;
    state = SNDST_PLAYING;
    if (update)
        Refresh();
}

// 0x10055210
void I3D_sound::Refresh()
{
    if (state == SNDST_STOPPED || !(frameFlags & FRMFLAGS_ON))
        return;
    if (soundType != I3DSOUND_AMBIENT && !g_renderCamera)
        return;
    if (!sound)
        return;
    if (state == SNDST_NO_CAMERA)
        SetOn(true);
    if (soundFlags & SNDF_TYPE) {
        soundFlags &= ~SNDF_TYPE;
        switch (soundType) {
        case I3DSOUND_NONE:
            if (sourceFlags & ISNDF_3D)
                audio::g_pAudio->SetMode(sound->buffer3D, DS3DMODE_DISABLE, DS3D_IMMEDIATE);
            SetOn(false);
            return;
        case I3DSOUND_POINT:
        case I3DSOUND_SPOT:
            audio::g_pAudio->SetMode(sound->buffer3D, DS3DMODE_NORMAL, DS3D_IMMEDIATE);
            break;
        case I3DSOUND_AMBIENT:
            if (sourceFlags & ISNDF_3D)
                audio::g_pAudio->SetMode(sound->buffer3D, DS3DMODE_DISABLE, DS3D_IMMEDIATE);
            break;
        default:
            break;
        }
    }
    if (soundType == I3DSOUND_NONE) {
        SetOn(false);
        return;
    }
    if (!IsPlaying(false)) {
        state = SNDST_STOPPED;
        soundFlags &= ~SNDF_PLAY;
        return;
    }
    if (soundFlags & (SNDF_RANGE | SNDF_SCALE)) {
        nearScaled = nearRange * worldScale;
        const float farS = farRange * worldScale;
        farScaled = farS;
        farScaledSq = farS * farS;
        const float limit = farS - 1e-8f;
        if (nearScaled >= limit)
            nearScaled = limit;
        soundFlags &= ~(SNDF_RANGE | SNDF_SCALE);
        midScaled = (farS - nearScaled) * midFraction + nearScaled;
    }

    S_vector toCamera(0.0f, 0.0f, 0.0f);
    float dist2 = 0.0f;
    if (soundType != I3DSOUND_AMBIENT) {
        const S_vector &cam = g_renderCamera->GetMatrix()(3);
        const S_vector &pos = GetMatrix()(3);
        toCamera = S_vector(cam.x - pos.x, cam.y - pos.y, cam.z - pos.z);
        dist2 = SquareZYX(toCamera);
        if (dist2 >= farScaledSq) {
            if (state != SNDST_OUT_OF_RANGE)
                StopOutOfRange();
            return;
        }
    }
    if (state == SNDST_OUT_OF_RANGE) {
        const uint32_t f = soundFlags;
        if (((f & SNDF_IN_CAMERA_SECTOR) || sectors.Size() == 0) &&
            ((f & SNDF_AREA_ACTIVE) || ((f & SNDF_AREA_LEAVING) && !IsTinyBits(areaFade)) || areas.Size() == 0))
            state = SNDST_AUDIBLE;
        if (state == SNDST_OUT_OF_RANGE)
            return;
    }

    // Distance attenuation: full to near, to midVolume at the middle point, to 0 at far.
    auto distanceFactor = [this](float dist) {
        if (dist <= nearScaled)
            return 1.0f;
        if (dist <= midScaled)
            return (1.0f - (dist - nearScaled) / (midScaled - nearScaled)) * (1.0f - midVolume) + midVolume;
        return (1.0f - (dist - midScaled) / (farScaled - midScaled)) * midVolume;
    };
    float v;
    switch (soundType) {
    case I3DSOUND_POINT:
        v = distanceFactor(std::sqrt(dist2));
        break;
    case I3DSOUND_SPOT: {
        S_vector dir = toCamera;
        NormalizeWithSquare(dir, SquareZYX(dir));
        S_vector fwd = GetMatrix()(2);
        NormalizeWithSquare(fwd, SquareYZX(fwd));
        const float dot = (fwd.x * dir.x + fwd.z * dir.z) + fwd.y * dir.y;
        float cone;
        if (dot < coneOuterCos || dot == coneOuterCos) {
            if (IsTinyBits(std::fabs(outVolume))) {
                if (state != SNDST_OUT_OF_RANGE)
                    StopOutOfRange();
                return;
            }
            cone = outVolume;
        } else if (dot > coneInnerCos) {
            cone = 1.0f;
        } else {
            cone = (1.0f - (dot - coneInnerCos) / (coneOuterCos - coneInnerCos)) * (1.0f - outVolume) + outVolume;
        }
        v = cone * distanceFactor(std::sqrt(dist2));
        break;
    }
    case I3DSOUND_AMBIENT:
        v = 1.0f;
        break;
    default:
        v = 0.0f;                       // (the original reads an unset local)
        break;
    }
    v = v * volume;

    if (envSector) {
        if ((envSector->sectorFlags & SCTF_ENV_FADING_OUT) && IsTinyBits(envSector->environmentBlend)) {
            envSector = nullptr;
            if (state != SNDST_OUT_OF_RANGE)
                StopOutOfRange();
            return;
        }
        if (!(envSector->sectorFlags & SCTF_ENV_FADING_OUT) && IsTinyBits(1.0f - envSector->environmentBlend)) {
            envSector = nullptr;
            return;
        }
        v = v * envSector->environmentBlend;
    }

    if (areas.Size() > 0) {
        if (areaFade >= 0.0f) {
            if ((soundFlags & SNDF_AREA_ACTIVE) && !IsTinyBits(1.0f - areaFade)) {
                const DWORD now = platform::Milliseconds();
                float f = (1.0f / static_cast<float>(areaFadeDuration)) *
                    static_cast<float>(static_cast<int32_t>(now - areaFadeTime)) + areaFade;
                if (!(f < 1.0f))
                    f = 1.0f;
                areaFade = f;
                areaFadeTime = now;
                v = f * v;
            } else if ((soundFlags & SNDF_AREA_LEAVING) && !IsTinyBits(areaFade)) {
                const DWORD now = platform::Milliseconds();
                float f = areaFade - (1.0f / static_cast<float>(areaFadeDuration)) *
                    static_cast<float>(static_cast<int32_t>(now - areaFadeTime));
                if (!(f > 0.0f))
                    f = 0.0f;
                areaFade = f;
                areaFadeTime = now;
                v = f * v;
            }
        }
        if (!(soundFlags & SNDF_AREA_ACTIVE) && IsTinyBits(areaFade)) {
            soundFlags &= ~SNDF_AREA_LEAVING;
            areaFade = -1.0f;
            if (state != SNDST_OUT_OF_RANGE)
                StopOutOfRange();
            return;
        }
        if (!(soundFlags & SNDF_AREA_LEAVING) && IsTinyBits(1.0f - areaFade))
            areaFade = -1.0f;
    }

    if ((soundFlags & SNDF_VOLUME) || std::fabs(lastVolume - v) > 0.01f) {
        lastVolume = v;
        sound->SetVolume(v);
    }
    if (soundFlags & SNDF_FREQUENCY)
        sound->SetFrequency(frequency);
    if (soundFlags & SNDF_POSITION)
        sound->SetPosition(GetMatrix()(3));
    if (soundFlags & SNDF_DIRECTION) {
        S_vector dir = GetMatrix()(2);
        NormalizeWithSquare(dir, SquareYZX(dir));
        sound->SetConeOrientation(dir);
    }
    if (soundFlags & SNDF_VELOCITY)
        sound->SetVelocity(*reinterpret_cast<const S_vector *>(&velocity));
    soundFlags &= ~SNDF_UPDATE_MASK;
    if (state == SNDST_AUDIBLE)
        SetOn(true);
    if (!(soundFlags & SNDF_PLAY))
        return;
    sound->SetVolume(lastVolume);
    sound->Play(playPosition != 0 ? (sourceFlags | ISNDP_KEEP_POSITION) : sourceFlags, priority);
    soundFlags &= ~SNDF_PLAY;
}

// 0x10055ec0
I3D_RESULT I3D_sound::Duplicate(I3D_frame *frame)
{
    if (this->type == frame->type) {
        I3D_sound *src = static_cast<I3D_sound *>(frame);
        SetOn(false);
        if (source) {
            source->Release();
            source = nullptr;
        }
        source = src->source;
        if (!source) {
            if (sound) {
                sound->Release();
                sound = nullptr;
            }
        } else {
            ++source->refCount;
            if (!sound && g_sndDriver->Create3DSound(&sound) < 0)
                return I3DERR_GENERIC;
            if (sound->Open(source, src->sourceFlags) < 0)
                return I3DERR_GENERIC;
        }
        loop = src->loop;
        outVolume = src->outVolume;
        volume = src->volume;
        frequency = src->frequency;
        sourceFlags = src->sourceFlags;
        nearRange = src->nearRange;
        farRange = src->farRange;
        midFraction = src->midFraction;
        midVolume = src->midVolume;
        soundType = src->soundType;
        coneInner = src->coneInner;
        coneOuter = src->coneOuter;
        if (sourceFlags & ISNDF_3D) {
            sound->SetDistances(1e9f, 1e9f);
            SetCone(1.04719758f, 1.57079637f);
        } else if (soundType != I3DSOUND_NONE && soundType != I3DSOUND_AMBIENT && source) {
            S_ISND_format format;
            source->GetFormat(&format);
            if (static_cast<uint32_t>(format.channels) < 2)
                dbgPrintf("WARNING ! I3D_sound::Duplicate() - Sound[%s], which isn't 3D, can be ambient only. Forcing to ambient type!",
                          SourceName(source));
            else
                dbgPrintf("WARNING ! I3D_sound::Duplicate() - STEREO sound[%s] can be ambient only. Forcing to ambient type!",
                          source->fileName.c_str());
            SetSoundType(I3DSOUND_AMBIENT);
        }
        if (frameFlags & FRMFLAGS_ON)
            SetOn(true);
        SetCurrTime(0);
        soundFlags = 0x21ff;
    }
    return I3D_frame::Duplicate(frame);
}

namespace ls3d::i3d {

// (ISND_driver::SetVolume, inlined there.)
void RefreshSoundVolumes()
{
    for (int i = g_driver->soundFrames.Size(); i != 0; --i) {
        I3D_sound *s = static_cast<I3D_sound *>(g_driver->soundFrames[i - 1]);
        if (s->IsPlaying(false)) {
            s->soundFlags |= SNDF_VOLUME;
            s->Refresh();
        }
    }
}

}  // namespace ls3d::i3d
