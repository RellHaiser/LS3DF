// I3D_sound: a sound source in the scene (FRAME_SOUND; vtable 0x1009bcb8, 0x1D0 bytes; methods
// 0x10054310 - 0x100560f0).
//
// A sound frame plays an ISND 3D sound of a source shared by name (the ISND driver's source list).
// Point (I3DSOUND_POINT) and spot sounds fade with the camera distance between the near range and
// the far range (full volume to near, the third range value the fraction of the way to the middle
// point, the fourth the volume there), spot sounds also by the angle between their direction and
// the camera (the cone); ambient sounds play at full volume. Sounds in sectors play while the
// camera is in one of them; sounds in areas while the camera is inside one (fading in and out over
// the area's fade time); sounds the camera's environment sector blends into follow its blend.
// Out of range a sound stops (SNDST_OUT_OF_RANGE) and resumes at the position it would have
// reached.
#pragma once

#include "core/c_vector.h"
#include "i3d/frame.h"

class I3D_area;
class I3D_sector;
class ISND_3Dsound;
class ISND_source;

// Sound types (SetSoundType).
enum : int {
    I3DSOUND_NONE = 0,                      // not played
    I3DSOUND_POINT = 1,
    I3DSOUND_SPOT = 2,                      // with a cone
    I3DSOUND_AMBIENT = 3,                   // no 3D position
};

// I3D_sound::soundFlags: what changed (sent to the ISND sound by Update) and where it plays.
enum : uint32_t {
    SNDF_POSITION = 0x0001,
    SNDF_DIRECTION = 0x0002,
    SNDF_RANGE = 0x0004,
    SNDF_VOLUME = 0x0008,
    SNDF_OUT_VOLUME = 0x0010,
    SNDF_FREQUENCY = 0x0020,
    SNDF_CONE = 0x0040,
    SNDF_SCALE = 0x0080,                    // the world scale changed (the ranges scale)
    SNDF_TYPE = 0x0100,
    SNDF_IN_CAMERA_SECTOR = 0x0200,         // in the sector the camera is in
    SNDF_AREA_ACTIVE = 0x0400,              // in an area the camera is inside (fading in)
    SNDF_AREA_LEAVING = 0x0800,             // the camera left its area (fading out)
    SNDF_PLAY = 0x1000,                     // Update starts the ISND sound
    SNDF_VELOCITY = 0x2000,
    SNDF_UPDATE_MASK = 0x0000207F,          // the flags Update clears after sending
};

// I3D_sound::state.
enum : uint8_t {
    SNDST_STOPPED = 1,
    SNDST_PLAYING = 2,
    SNDST_OUT_OF_RANGE = 3,                 // stopped until audible again (stopTime, playPosition kept)
    SNDST_AUDIBLE = 4,                      // audible again: Update restarts it
    SNDST_NO_CAMERA = 5,                    // switched on before a camera rendered
};

class I3D_sound : public I3D_frame {
public:
    // 0x30 0x1001bb10: SetActive(on, false).
    void __stdcall SetOn(bool on) override;
    // 0x4C 0x10055ec0: also the source and the settings of another sound.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10054310 / 0x100544b0
    void Init() override;
    void Uninit() override;
    // 0x70 0x100560f0: (the frame only).
    void Load() override;
    // 0x74 0x100545a0: the sound file (ISND_source flags; 3D added), shared with sounds of the
    // same name.
    virtual I3D_RESULT __stdcall Open(const char *fileName, uint32_t flags);
    // 0x78 0x100548d0: `device`: the ISND sound's; else whether the frame plays (out of range:
    // whether it would still be playing).
    virtual bool __stdcall IsPlaying(bool device);
    virtual void __stdcall SetSoundType(int newType);                      // 0x7C 0x100549a0
    virtual void __stdcall SetRange(float nearRange, float farRange, float midFraction,
                                    float midVolume);                   // 0x80 0x10054a40
    // 0x84 0x10054b20: the cone angles (radians, at most pi).
    virtual void __stdcall SetCone(float inner, float outer);
    virtual void __stdcall SetOutVolume(float volume);                  // 0x88 0x10054c00
    virtual void __stdcall SetVolume(float volume);                     // 0x8C 0x10054c40
    virtual void __stdcall SetLoop(bool loop);                          // 0x90 0x10054c80
    // 0x94 0x10054cd0: the play position (ms; ISND_PAUSE / ISND_RESUME go to the ISND sound).
    virtual I3D_RESULT __stdcall SetCurrTime(int position);
    virtual void __stdcall SetFrequency(float ratio);                   // 0x98 0x10054d50 (0.01 - 100)
    virtual void __stdcall GetRange(float &nearRange, float &farRange, float &midFraction,
                                    float &midVolume);                  // 0x9C 0x10054e80
    virtual void __stdcall GetCone(float &inner, float &outer);         // 0xA0 0x10054ec0
    virtual int __stdcall GetCurrTime();                                // 0xA4 0x10054ee0
    virtual int __stdcall GetLength();                                  // 0xA8 0x10054f00
    // 0xAC 0x10054f30: starts / stops; `update`: Update at once.
    virtual void __stdcall SetActive(bool on, bool update);
    virtual void __stdcall SetVelocity(const S_vector4 &velocity);      // 0xB0 0x10054dc0
    virtual void __stdcall UpdateSound();                               // 0xB4 0x10055200

    // The volume and 3D parameters sent to the ISND sound (0x10055210; Render calls it for the
    // driver's sound frames).
    void Refresh();
    // Out of range: stops the ISND sound and keeps its position (inlined in the original).
    void StopOutOfRange();

    uint32_t soundFlags;                    // +0x120 SNDF_*
    uint8_t state;                          // +0x124 SNDST_*
    uint8_t padding125[3];
    uint32_t priority;                      // +0x128 (Play's)
    int changeBase;                         // +0x12C (0, never changed: Render resets changeCount to it)
    uint32_t unknown130;                    // +0x130
    uint32_t stopTime;                      // +0x134 timeGetTime when it went out of range
    int playPosition;                       // +0x138 ms
    ISND_3Dsound *sound;                    // +0x13C
    ISND_source *source;                    // +0x140
    bool loop;                              // +0x144
    uint8_t padding145[3];
    float nearRange;                        // +0x148
    float nearScaled;                       // +0x14C x the world scale (below farScaled)
    float farRange;                         // +0x150
    float farScaled;                        // +0x154
    float farScaledSq;                      // +0x158
    float midFraction;                      // +0x15C
    float midScaled;                        // +0x160 nearScaled + (farScaled - nearScaled) x midFraction
    float midVolume;                        // +0x164
    float outVolume;                        // +0x168 outside the cone
    float volume;                           // +0x16C
    float lastVolume;                       // +0x170 sent to the ISND sound
    float frequency;                        // +0x174
    uint32_t sourceFlags;                   // +0x178 the source's ISNDF_* (+ ISNDP_LOOP: SetLoop)
    int soundType;                          // +0x17C I3DSOUND_*
    float coneInner;                        // +0x180
    float coneInnerCos;                     // +0x184 cos(inner / 2)
    float coneOuter;                        // +0x188
    float coneOuterCos;                     // +0x18C
    I3D_sector *envSector;                  // +0x190 (renderer) the fading environment sector it follows
    float areaFade;                         // +0x194 0 - 1, -1: none
    uint32_t areaFadeTime;                  // +0x198 timeGetTime of the last step
    uint32_t areaFadeDuration;              // +0x19C ms (the area's fade time)
    S_vector4 velocity;                     // +0x1A0
    ls3d::C_vector<I3D_sector *> sectors;   // +0x1B0 the sectors it was added to (AddSound)
    ls3d::C_vector<I3D_area *> areas;       // +0x1C0 the areas it was added to (I3D_area::AddSound)
};
static_assert(sizeof(I3D_sound) == 0x1D0, "I3D_sound layout");
static_assert(offsetof(I3D_sound, sound) == 0x13C, "I3D_sound layout");
static_assert(offsetof(I3D_sound, envSector) == 0x190, "I3D_sound layout");
static_assert(offsetof(I3D_sound, sectors) == 0x1B0, "I3D_sound layout");
