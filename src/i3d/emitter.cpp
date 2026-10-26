// I3D_emitor and the particles' shared buffers (0x1003ab60 - 0x1003ed10; 0x1003ab60, 0x1003ab90,
// 0x1003abc0 / 0x1003abe0, 0x1003e790 and 0x1003ebf0 are the static initialisers (and an atexit
// destructor) of this file's globals: header copies of a zero vector and two (0, 0, 0, 1) vectors,
// the default texture rectangle and the sort keys).
#include "i3d/emitter.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

#include "core/debug.h"
#include "core/fpu.h"
#include "external/rw_data.h"
#include "i3d/camera.h"
#include "i3d/driver.h"
#include "i3d/i3d_links.h"
#include "i3d/loader.h"
#include "i3d/material.h"
#include "i3d/mesh.h"
#include "i3d/object.h"
#include "i3d/render_helpers.h"
#include "i3d/render_passes.h"
#include "i3d/render_state.h"
#include "igraph/igraph.h"
#include "igraph/igraph_state.h"
#include "igraph/itexture.h"

using namespace ls3d;
using namespace ls3d::i3d;
using gfx::g_pGfx;
using ig::g_rs;

namespace {

// Quads per shared vertex buffer (0x5554 vertices): the write position at kMaxQuads means the
// buffer in turn has not been locked since it was drawn.
constexpr int kMaxQuads = 0x1555;
// The particle count an emitter stops letting out at (one more than a buffer's quads).
constexpr int kMaxParticles = 0x1556;

// The quads' index buffer (0x100be6c0) and the two vertex buffers (0x100be6f0) used in turn
// (0x100be718), the lock of the current one (0x100be714) and its write position in quads
// (0x100a6898).
S_IBAlloc s_ib;
S_VBAlloc s_vbs[2];
int s_vbIndex;
uint8_t *s_lockedVertices;
int s_writeQuad = kMaxQuads;

// The depth sort keys of an emitter's particles (0x100be708; freed at exit).
struct S_particleKeys {
    S_sortKey *keys = nullptr;
    uint32_t capacity = 0;
    uint32_t count = 0;

    ~S_particleKeys() { ::operator delete(keys); }
    // Room for `n` keys, the old ones copied (0x1003eb90, shared with the render queue's keys;
    // false when out of memory).
    bool Grow(uint32_t n)
    {
        auto *p = static_cast<S_sortKey *>(::operator new(sizeof(S_sortKey) * n, std::nothrow));
        if (!p)
            return false;
        for (uint32_t i = 0; i < count; ++i)
            p[i] = keys[i];
        capacity = n;
        ::operator delete(keys);
        keys = p;
        return true;
    }
};
S_particleKeys s_keys;

// The texture rectangle of a particle without a group (0x100be6cc).
const S_vector4 kWholeTexture = { 0.0f, 0.0f, 1.0f, 1.0f };

S_vector &AsVector(S_vector4 &v) { return reinterpret_cast<S_vector &>(v); }
const S_vector &AsVector(const S_vector4 &v) { return reinterpret_cast<const S_vector &>(v); }

float FloatBits(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}
uint32_t BitsOf(float f)
{
    uint32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

// r, g and b clamped to 0 - 1 (NaN kept); w as given.
void ClampColor(S_vector4 &c)
{
    for (float *f : { &c.x, &c.y, &c.z }) {
        if (*f < 0.0f)
            *f = 0.0f;
        else if (*f > 1.0f)
            *f = 1.0f;
    }
}

// The random wait for the next particle (the SDK's `min + rand x random` truncated).
uint32_t RandomInterval(int min, int random)
{
    const float r = RandFloat() * static_cast<float>(random);
    return static_cast<uint32_t>(Ftol(r + static_cast<float>(min)));
}

// The queued item of an emitter's quads (blended; the depth bits or'ed in by the caller).
S_renderItem &QueueItem(I3D_emitor *e)
{
    S_renderItem &item = g_rc.items.PushBackUninit();
    item.visual = e;
    item.sector = g_rc.sector;
    item.texture = e->texture;
    item.sortKey = RIKEY_BLENDED;
    ++g_rc.numBlended;
    return item;
}

}  // namespace

// ---------------------------------------------------------------- the shared buffers

namespace ls3d::i3d {

// 0x1003ac00
int InitParticleBuffers()
{
    FreeIB(&s_ib);
    for (S_VBAlloc &vb : s_vbs) {
        FreeVB(&vb);
        vb = {};
    }
    s_ib = {};
    if (!g_ibManager.Alloc(D3DUSAGE_SOFTWAREPROCESSING, 0x7ffe, &s_ib))
        return I3DERR_OUTOFMEM;
    // Two triangles per quad: 0 1 2, 2 1 3. (The buffer stays locked until the renderer unlocks
    // the managers' buffers.)
    uint16_t *index = s_ib.buffer->Lock(0) + s_ib.offset;
    for (int i = 0; i < kMaxQuads; ++i) {
        const uint16_t base = static_cast<uint16_t>(i * 4);
        index[0] = base;
        index[1] = static_cast<uint16_t>(base + 1);
        index[2] = static_cast<uint16_t>(base + 2);
        index[3] = static_cast<uint16_t>(base + 2);
        index[4] = static_cast<uint16_t>(base + 1);
        index[5] = static_cast<uint16_t>(base + 3);
        index += 6;
    }
    for (S_VBAlloc &vb : s_vbs) {
        vb = {};
        if (!g_vbManager.Alloc(D3DUSAGE_WRITEONLY, sizeof(S_particleVertex), kMaxQuads * 4, &vb, 2, false))
            return I3DERR_OUTOFMEM;
        if (vb.offset != 0)
            dbgPrintf("!! ERROR !!!  InitParticle() - StartIndex != 0 !!!!!");
    }
    return I3D_OK;
}

// 0x1003add0
void ReleaseParticleBuffers()
{
    FreeIB(&s_ib);
    // (Fix: the original keeps the index buffer's record, which InitParticleBuffers frees again:
    // by then the block may be another mesh's, or its buffer gone.)
    s_ib = {};
    for (S_VBAlloc &vb : s_vbs) {
        FreeVB(&vb);
        vb = {};
    }
}

// 0x1003ae10
void FlushParticles()
{
    if (s_lockedVertices) {
        g_pGfx->Unlock(s_vbs[s_vbIndex].vb);
        s_lockedVertices = nullptr;
    }
}

// 0x1003ae40: after the frame the next buffer takes its turn (when this one was written).
void EndParticles()
{
    if (s_writeQuad > 0 && s_writeQuad != kMaxQuads) {
        if (++s_vbIndex > 1)
            s_vbIndex = 0;
        s_writeQuad = kMaxQuads;
    }
}

}  // namespace ls3d::i3d

// ---------------------------------------------------------------- S_emitorGroup

S_emitorGroup &S_emitorGroup::operator=(const S_emitorGroup &src)
{
    rect = src.rect;
    columns = src.columns;
    rows = src.rows;
    numFrames = src.numFrames;
    loopFrame = src.loopFrame;
    frameTime = src.frameTime;
    // (The original frees its frames first, so assigning a group to itself empties them; the
    // port never does that.)
    frames.Free();
    const int n = src.frames.Size();
    frames.ResizeDiscard(n);
    for (int i = n - 1; i >= 0; --i)
        frames[i] = src.frames[i];
    loopStart = src.loopStart;
    loopLength = src.loopLength;
    return *this;
}

// ---------------------------------------------------------------- S_particle

void S_particle::Update(int time, I3D_emitor *e, int activeIndex)
{
    age += static_cast<uint32_t>(time);
    if (age >= life) {
        e->freeSlots.PushBack(e->active[activeIndex]);
        e->active.RemoveAtSwap(activeIndex);
        return;
    }
    while (time > 0) {
        const int step = time < 10 ? time : 10;
        time -= 10;
        if (turnInterval > 0 && (turnTime += step) >= turnInterval) {
            turnTime = 0;
            turnInterval = static_cast<int>(RandomInterval(turnIntervalMin, turnIntervalRandom));
            prevDir = dir;
            dir = endDir;
            for (float *f : { &dir.x, &dir.y, &dir.z }) {
                const float half = turnSpread * 0.5f;
                *f = (half - RandFloat() * turnSpread) + *f;
            }
            AsVector(dir).Normalize();
        }
        const float fstep = static_cast<float>(step);
        scale = (fstep * growth + 1.0f) * scale;
        if (scale < 0.0f)
            scale = 0.0f;
        const uint32_t a = age;
        if (a < fadeInEnd)
            alpha = static_cast<float>(static_cast<int>(a)) / static_cast<float>(static_cast<int>(fadeInEnd));
        else if (a > fadeOutStart)
            alpha = static_cast<float>(life - a) / static_cast<float>(static_cast<int>(life - fadeOutStart));
        else
            alpha = 1.0f;
        alpha = alpha * opacity;
        const float fa = static_cast<float>(static_cast<int>(a));
        const float t = fa / static_cast<float>(life);
        speed = (endSpeed - startSpeed) * t + startSpeed;
        color.x = (endColor.x - startColor.x) * t + startColor.x;
        color.y = (endColor.y - startColor.y) * t + startColor.y;
        color.z = t * (endColor.z - startColor.z) + startColor.z;
        // How far the direction has turned: through the current turn, or over the longest life.
        float blend;
        if (turnInterval > 0) {
            int n = maxLife - static_cast<int>(a);
            if (turnInterval < n)
                n = turnInterval;
            blend = static_cast<float>(turnTime) / static_cast<float>(n);
        } else {
            blend = fa / static_cast<float>(maxLife);
        }
        const float inv = 1.0f - blend;
        const float k = (speed * fstep) * 0.001f;
        position.x = (blend * dir.x + inv * prevDir.x) * k + position.x;
        position.y = (blend * dir.y + inv * prevDir.y) * k + position.y;
        position.z = (blend * dir.z + inv * prevDir.z) * k + position.z;
    }
}

// ---------------------------------------------------------------- I3D_emitor

// (The driver's factory, 0x10019750: the vectors' grow steps.)
I3D_emitor::I3D_emitor()
    : groups(nullptr), groupCapacity(0), numGroups(0), groupGrowBy(2), particles(nullptr), particleCapacity(0),
      numParticles(0), particleGrowBy(1), active(0x20), freeSlots(0x20)
{
}

// (The original's visuals share a destructor that frees none of this, at driver shutdown.)
I3D_emitor::~I3D_emitor()
{
    FreeGroups();
    AlignedFree(particles);
}

void I3D_emitor::FreeGroups()
{
    delete[] groups;                        // 0x1003e8e0
    groups = nullptr;
}

void I3D_emitor::Init()
{
    I3D_visual::Init();
    texture = nullptr;
    SetDefaults();
    emitTime = 0;
    emitWait = 0;
    numEmitted = 0;
    active.SetGrowBy(0x32);
    freeSlots.SetGrowBy(0x32);
    particleGrowBy = 0x32;
    firstQuad = -1;
    fixedGroup = -1;
    userVertices = false;
    userQuads = 0;
    userDepth = 0.0f;
    srcBlend = D3DBLEND_SRCALPHA;
    destBlend = D3DBLEND_INVSRCALPHA;
    visualFlags |= VISF_NO_ZWRITE;
}

// (The groups and the storage stay for the next use.)
void I3D_emitor::Uninit()
{
    if (texture)
        texture->Release();
    texture = nullptr;                      // (fix: the original leaves it dangling until Init)
    numParticles = 0;
    active.Clear();
    freeSlots.Clear();
    numGroups = 0;
    I3D_frame::Uninit();
}

void I3D_emitor::SetTexture(ITexture *newTexture)
{
    if (newTexture == texture)
        return;
    if (texture)
        texture->Release();
    if (newTexture)
        ++newTexture->refCount;
    texture = newTexture;
}

I3D_RESULT I3D_emitor::SetParam(int index, uint32_t value)
{
    if (index >= EMTP_COUNT) {
        dbgPrintf("!! Invalid index:%u  I3D_emitor::SetParam()", index);
        return I3DERR_INVALIDPARAMS;
    }
    const float f = FloatBits(value);
    switch (index) {
    case EMTP_INTERVAL:
        intervalMin = static_cast<int>(value);
        emitWait = 0;
        break;
    case EMTP_INTERVAL_RANDOM:
        intervalRandom = static_cast<int>(value);
        emitWait = RandomInterval(intervalMin, intervalRandom);
        break;
    case EMTP_LIFE: lifeMin = static_cast<int>(value); break;
    case EMTP_LIFE_RANDOM: lifeRandom = static_cast<int>(value); break;
    case EMTP_GROWTH_A: growthA = f; break;
    case EMTP_GROWTH_B: growthB = f; break;
    case EMTP_POSITION_SPREAD: positionSpread = f; break;
    case EMTP_DIRECTION_SPREAD: directionSpread = f; break;
    case EMTP_TURN_INTERVAL: turnIntervalMin = static_cast<int>(value); break;
    case EMTP_TURN_INTERVAL_RANDOM: turnIntervalRandom = static_cast<int>(value); break;
    case EMTP_TURN_SPREAD: turnSpread = f; break;
    case EMTP_FADE_IN: fadeIn = f; break;
    case EMTP_FADE_OUT: fadeOut = f; break;
    case EMTP_START_VELOCITY_X: startVelocity.x = f; break;
    case EMTP_START_VELOCITY_X + 1: startVelocity.y = f; break;
    case EMTP_START_VELOCITY_X + 2: startVelocity.z = f; break;
    case EMTP_END_VELOCITY_X: endVelocity.x = f; break;
    case EMTP_END_VELOCITY_X + 1: endVelocity.y = f; break;
    case EMTP_END_VELOCITY_X + 2: endVelocity.z = f; break;
    case EMTP_FLAGS: emitFlags = value & EMITF_MASK; break;
    case EMTP_MAX_PARTICLES:
        maxEmitted = static_cast<int>(value);
        emitting = true;
        break;
    case EMTP_START_VELOCITY_WORLD: startVelocityWorld = static_cast<uint8_t>(value) != 0; break;
    case EMTP_END_VELOCITY_WORLD: endVelocityWorld = static_cast<uint8_t>(value) != 0; break;
    case EMTP_WARM_UP:
        warmUp = static_cast<int>(value);
        if (value)
            Tick(static_cast<int>(value));
        break;
    case EMTP_ROTATION_MIN: rotationMin = f; break;
    case EMTP_ROTATION_MAX: rotationMax = f; break;
    case EMTP_START_SCALE: startScale = f; break;
    case EMTP_START_COLOR:
    case EMTP_END_COLOR: {
        const auto *c = reinterpret_cast<const S_vector4 *>(static_cast<uintptr_t>(value));
        if (!c)
            return I3DERR_INVALIDPARAMS;
        S_vector4 &dst = index == EMTP_START_COLOR ? startColor : endColor;
        dst = *c;
        ClampColor(dst);
        break;
    }
    case EMTP_OPACITY:
        opacity = f;
        if (f > 1.0f)
            opacity = 1.0f;
        if (opacity < 0.0f)
            opacity = 0.0f;
        break;
    default:
        break;
    }
    return I3D_OK;
}

uint32_t I3D_emitor::GetParam(int index)
{
    if (index >= EMTP_COUNT) {
        dbgPrintf("!! Invalid index:%u  I3D_emitor::GetParam()", index);
        return 0;
    }
    switch (index) {
    case EMTP_INTERVAL: return static_cast<uint32_t>(intervalMin);
    case EMTP_INTERVAL_RANDOM: return static_cast<uint32_t>(intervalRandom);
    case EMTP_LIFE: return static_cast<uint32_t>(lifeMin);
    case EMTP_LIFE_RANDOM: return static_cast<uint32_t>(lifeRandom);
    case EMTP_GROWTH_A: return BitsOf(growthA);
    case EMTP_GROWTH_B: return BitsOf(growthB);
    case EMTP_POSITION_SPREAD: return BitsOf(positionSpread);
    case EMTP_DIRECTION_SPREAD: return BitsOf(directionSpread);
    case EMTP_TURN_INTERVAL: return static_cast<uint32_t>(turnIntervalMin);
    case EMTP_TURN_INTERVAL_RANDOM: return static_cast<uint32_t>(turnIntervalRandom);
    case EMTP_TURN_SPREAD: return BitsOf(turnSpread);
    case EMTP_FADE_IN: return BitsOf(fadeIn);
    case EMTP_FADE_OUT: return BitsOf(fadeOut);
    case EMTP_START_VELOCITY_X: return BitsOf(startVelocity.x);
    case EMTP_START_VELOCITY_X + 1: return BitsOf(startVelocity.y);
    case EMTP_START_VELOCITY_X + 2: return BitsOf(startVelocity.z);
    case EMTP_END_VELOCITY_X: return BitsOf(endVelocity.x);
    case EMTP_END_VELOCITY_X + 1: return BitsOf(endVelocity.y);
    case EMTP_END_VELOCITY_X + 2: return BitsOf(endVelocity.z);
    case EMTP_FLAGS: return emitFlags;
    case EMTP_MAX_PARTICLES: return static_cast<uint32_t>(maxEmitted);
    case EMTP_START_VELOCITY_WORLD: return startVelocityWorld;
    case EMTP_END_VELOCITY_WORLD: return endVelocityWorld;
    case EMTP_WARM_UP: return static_cast<uint32_t>(warmUp);
    case EMTP_ROTATION_MIN: return BitsOf(rotationMin);
    case EMTP_ROTATION_MAX: return BitsOf(rotationMax);
    case EMTP_START_SCALE: return BitsOf(startScale);
    case EMTP_NUM_PARTICLES: return static_cast<uint32_t>(numParticles);
    case EMTP_START_COLOR: return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&startColor));
    case EMTP_END_COLOR: return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&endColor));
    case EMTP_OPACITY: return BitsOf(opacity);
    default: return 0;
    }
}

void I3D_emitor::SetDefaults()
{
    fadeIn = 0.5f;
    fadeOut = 0.5f;
    // (The velocities' w is left as the original's stack had it; the port sets 0.)
    startVelocity = { 0.0f, 2.0f, 0.0f, 0.0f };
    positionSpread = 0.0f;
    directionSpread = 0.0f;
    turnIntervalMin = 0;
    turnIntervalRandom = 0;
    maxEmitted = 0;
    startVelocityWorld = false;
    endVelocityWorld = false;
    warmUp = 0;
    endVelocity = { 2.0f, 0.0f, 0.0f, 0.0f };
    intervalMin = 100;
    intervalRandom = 60;
    lifeMin = 1800;
    lifeRandom = 600;
    startScale = 1.0f;
    growthA = -0.25f;
    growthB = 0.6f;
    turnSpread = 0.1f;
    rotationMin = -1.2566371f;
    rotationMax = 1.2566371f;
    startColor = { 1.0f, 1.0f, 1.0f, 0.0f };     // (the original's global (1, 1, 1) at 0x100d1570)
    endColor = { 1.0f, 1.0f, 1.0f, 0.0f };
    emitFlags = EMITF_SORTED | EMITF_BLEND;
    emitting = true;
    opacity = 1.0f;
}

void I3D_emitor::EmitOn(bool on)
{
    emitting = on;
    if (on) {
        numEmitted = 0;
        emitWait = 0;
        emitTime = 0;
    }
}

int I3D_emitor::AddGroup(const S_emitorGroup &desc, int index)
{
    S_emitorGroup g;
    g.rect = desc.rect;
    g.columns = desc.columns < 1 ? 1 : desc.columns;
    g.rows = desc.rows < 1 ? 1 : desc.rows;
    g.numFrames = desc.numFrames;
    if (g.numFrames < 1 || g.numFrames > g.rows * g.columns)
        g.numFrames = g.rows * g.columns;
    g.loopFrame = desc.loopFrame;
    // (Quirk kept: a loop frame past the frames takes one frame off the end, not the loop frame
    // back; Prepare guards the frame index.)
    if (g.loopFrame >= g.numFrames)
        --g.numFrames;
    g.frameTime = desc.frameTime;
    g.loopStart = g.frameTime * g.numFrames;
    g.loopLength = (g.numFrames - g.loopFrame) * g.frameTime;
    if (desc.frames.Size() > 0) {
        const int n = desc.frames.Size();
        g.frames.Free();
        g.frames.ResizeDiscard(n);
        for (int i = n - 1; i >= 0; --i)
            g.frames[i] = desc.frames[i];
    } else {
        // The grid row by row.
        const float du = (desc.rect.z - desc.rect.x) / static_cast<float>(g.columns);
        const float dv = (desc.rect.w - desc.rect.y) / static_cast<float>(g.rows);
        S_vector4 r = { desc.rect.x, desc.rect.y, desc.rect.x + du, desc.rect.y + dv };
        g.frames.Reserve(g.numFrames);
        int column = 1;
        for (int k = g.numFrames; k > 0; --k) {
            g.frames.PushBack(r);
            if (++column > g.columns) {
                column = 1;
                r.x = desc.rect.x;
                r.z = desc.rect.x + du;
                r.y = r.w;
                r.w = r.w + dv;
            } else {
                r.x = r.z;
                r.z = r.z + du;
            }
        }
    }
    if (index > -1) {
        // (Fix: the original writes past the groups for an index beyond them; within the array's
        // capacity that slot was unused, so ignoring it is the same.)
        if (index < numGroups) {
            S_emitorGroup &dst = groups[index];
            dst = g;
        }
        return index;
    }
    if (numGroups >= groupCapacity) {
        const int capacity = groupCapacity + groupGrowBy;
        auto *grown = new (std::nothrow) S_emitorGroup[capacity];
        if (!grown)                         // (the original goes on with a null array)
            return -1;
        groupCapacity = capacity;
        for (int i = 0; i < numGroups; ++i)
            grown[i] = groups[i];
        FreeGroups();
        groups = grown;
    }
    groups[numGroups++] = g;
    return numGroups - 1;
}

void I3D_emitor::RemoveGroup(int index)
{
    // (Fix: the original takes any index up to numGroups (also negative ones) and copies the last
    // group to it: for numGroups itself past the array. The port only removes the last one then.)
    if (index < 0 || index > numGroups)
        return;
    --numGroups;
    if (index < numGroups)
        groups[index] = groups[numGroups];
}

void I3D_emitor::ClearParticles()
{
    freeSlots.Clear();
    active.Clear();
    numParticles = 0;
}

S_particle *I3D_emitor::NewParticleSlot()
{
    if (numParticles >= particleCapacity) {
        const int capacity = particleCapacity + particleGrowBy;
        auto *grown = static_cast<S_particle *>(AlignedAlloc(sizeof(S_particle) * capacity, 16));
        if (!grown)                         // (fix: the original writes through the null block)
            return nullptr;
        particleCapacity = capacity;
        for (int i = 0; i < numParticles; ++i)
            grown[i] = particles[i];
        AlignedFree(particles);
        particles = grown;
    }
    return &particles[numParticles++];
}

void I3D_emitor::Tick(int time)
{
    for (int i = active.Size(); i != 0;) {
        --i;
        particles[active[i]].Update(time, this, i);
    }
    if (emitting) {
        emitTime += static_cast<uint32_t>(time);
        while (emitTime >= emitWait) {
            if (active.Size() == kMaxParticles) {
                emitTime = 0;
                break;
            }
            emitTime -= emitWait;
            ++numEmitted;
            if (maxEmitted && numEmitted > maxEmitted) {
                emitting = false;
                break;
            }
            // A free slot, else a new one.
            int slot;
            S_particle *p;
            if (freeSlots.Size() > 0) {
                slot = freeSlots[freeSlots.Size() - 1];
                active.PushBack(slot);
                freeSlots.PopBack();
                p = &particles[slot];
            } else {
                slot = numParticles;
                if (!(p = NewParticleSlot()))
                    break;
                active.PushBack(slot);
            }
            if (fixedGroup == -1)
                p->group = numGroups > 1 ? std::rand() % numGroups : 0;
            else
                p->group = fixedGroup;
            p->scale = startScale;
            const S_matrix &m = GetMatrix();
            p->position = reinterpret_cast<const S_vector4 &>(m.m[3]);
            for (float *f : { &p->position.x, &p->position.y, &p->position.z }) {
                const float r = RandFloat();
                *f = (r * positionSpread + positionSpread * -0.5f) + *f;
            }
            // The velocities: in the frame's space unless given in world space. (The w components
            // are unused; the original copies stack garbage into the rotated ones.)
            const S_matrix &w = GetMatrix();
            if (startVelocityWorld) {
                p->startDir = startVelocity;
            } else {
                AsVector(p->startDir) = AsVector(startVelocity).RotateByMatrix(w);
                p->startDir.w = startVelocity.w;
            }
            if (endVelocityWorld) {
                p->endDir = endVelocity;
            } else {
                AsVector(p->endDir) = AsVector(endVelocity).RotateByMatrix(w);
                p->endDir.w = endVelocity.w;
            }
            p->startSpeed = MagnitudeOfSquare(SquareXYZ(AsVector(startVelocity)));
            p->speed = p->startSpeed;
            p->endSpeed = MagnitudeOfSquare(SquareXYZ(AsVector(endVelocity)));
            AsVector(p->startDir).Normalize();
            AsVector(p->endDir).Normalize();
            p->prevDir = p->startDir;
            p->dir = p->endDir;
            for (float *f : { &p->dir.x, &p->dir.y, &p->dir.z }) {
                const float half = directionSpread * 0.5f;
                *f = (half - RandFloat() * directionSpread) + *f;
            }
            AsVector(p->dir).Normalize();
            p->age = 0;
            p->life = RandomInterval(lifeMin, lifeRandom);
            p->maxLife = lifeRandom + lifeMin;
            p->turnTime = 0;
            p->turnInterval = static_cast<int>(RandomInterval(turnIntervalMin, turnIntervalRandom));
            const float life = static_cast<float>(p->life);
            const float fadeInTime = life * fadeIn;
            p->fadeInEnd = static_cast<uint32_t>(Ftol(fadeInTime));
            const float fadeOutTime = life - life * fadeOut;
            p->fadeOutStart = static_cast<uint32_t>(Ftol(fadeOutTime));
            // A negative fade in starts at a random opacity (up to the cap).
            if (!(fadeIn < 0.0f)) {
                p->alpha = 0.0f;
            } else {
                const float a = (RandFloat() + 1.0f) * 0.5f;
                p->alpha = a > opacity ? opacity : a;
            }
            p->color = AsVector(startColor);
            p->opacity = opacity;
            p->turnIntervalMin = turnIntervalMin;
            p->turnIntervalRandom = turnIntervalRandom;
            p->turnSpread = turnSpread;
            p->startColor = startColor;
            p->endColor = endColor;
            {
                const float r = RandFloat();
                p->rotationSpeed = ((rotationMax - rotationMin) * r + rotationMin) * 0.001f;
            }
            {
                const float lo = growthB > growthA ? growthA : growthB;
                const float hi = growthB < growthA ? growthA : growthB;
                const float r = RandFloat();
                p->growth = ((hi - lo) * r + lo) * 0.001f;
            }
            const int interval = intervalMin;
            emitWait = RandomInterval(intervalMin, intervalRandom);
            // The time since the particle was due, roughly: what is left over the base interval.
            if (emitTime > emitWait) {
                const int late = static_cast<int>(emitTime - static_cast<uint32_t>(interval));
                // (Fix: the original passes the particle's slot as its active-list index, so a
                // particle dying here takes another one off the list, or writes past it.)
                if (late > 0)
                    p->Update(late, this, active.Size() - 1);
            }
        }
    }
    UpdateParticleBound();
}

I3D_RESULT I3D_emitor::Duplicate(I3D_frame *source)
{
    if (!source || source == this)
        return I3DERR_INVALIDPARAMS;
    I3D_frame::Duplicate(source);
    if (source->type != FRAME_VISUAL)
        return I3D_OK;
    auto *srcVisual = static_cast<I3D_visual *>(source);
    visualFlags = srcVisual->visualFlags & 0x2ff52;
    localBound = srcVisual->localBound;
    if (srcVisual->visualFlags & VISF_WORLD_SPACE) {
        srcVisual->GetWorldBound();
        worldBound = srcVisual->worldBound;
    } else {
        visualFlags &= ~VISF_WBOUND_VALID;
    }
    visualFlags = (visualFlags & ~VISF_OBJECT_PARAMS) | VISF_NO_ZWRITE;
    if (srcVisual->GetVisualType() != FourCC("EMTR")) {
        if (srcVisual->visualFlags & VISF_OBJECT_PARAMS) {
            ITexture *t = nullptr;
            if (I3D_mesh *mesh = static_cast<I3D_object *>(srcVisual)->mesh) {
                if (I3D_mesh_level *level = mesh->GetLevel(0)) {
                    if (level->faceGroups) {
                        if (I3D_material *mat = level->faceGroups[0].material) {
                            if (mat->matFlags & MATF_ANIMATED)
                                mat->UpdateTexture();
                            t = mat->texture;
                        }
                    }
                }
            }
            SetTexture(t);
        }
        return I3D_OK;
    }
    auto *src = static_cast<I3D_emitor *>(source);
    SetTexture(src->texture);
    FreeGroups();
    groupCapacity = 0;
    numGroups = 0;
    if (src->numGroups > 0) {
        groups = new (std::nothrow) S_emitorGroup[src->numGroups];       // 0x1003e940
        if (groups) {
            groupCapacity = src->numGroups;
            numGroups = src->numGroups;
            for (int i = numGroups - 1; i >= 0; --i)
                groups[i] = src->groups[i];
        }
    }
    fixedGroup = src->fixedGroup;
    intervalMin = src->intervalMin;
    intervalRandom = src->intervalRandom;
    lifeMin = src->lifeMin;
    lifeRandom = src->lifeRandom;
    startScale = src->startScale;
    growthA = src->growthA;
    growthB = src->growthB;
    positionSpread = src->positionSpread;
    directionSpread = src->directionSpread;
    fadeIn = src->fadeIn;
    fadeOut = src->fadeOut;
    turnIntervalMin = src->turnIntervalMin;
    turnIntervalRandom = src->turnIntervalRandom;
    turnSpread = src->turnSpread;
    startVelocity = src->startVelocity;
    endVelocity = src->endVelocity;
    emitFlags = src->emitFlags;
    maxEmitted = src->maxEmitted;
    startVelocityWorld = src->startVelocityWorld;
    endVelocityWorld = src->endVelocityWorld;
    rotationMin = src->rotationMin;
    rotationMax = src->rotationMax;
    warmUp = src->warmUp;
    startColor = src->startColor;
    endColor = src->endColor;
    // (The opacity and the custom blend factors are not copied.)
    numEmitted = 0;
    emitWait = 0;
    emitTime = 0;
    emitting = src->emitting;
    return I3D_OK;
}

void I3D_emitor::Load()
{
    I3D_frame::Load();
    uint32_t flags = 0;
    dtaRead(g_loadFile, &flags, 4);
    // The texture flags as a material's (I3D_material::TextureFlags).
    uint32_t tf = flags >> 5 & ITC_STREAMED;
    if (g_driverStates[DRVSTATE_TEX_DITHER])
        tf |= ITC_DITHER;
    if (g_driverStates[DRVSTATE_TEX_TRUECOLOR] || (flags & 0x100000))
        tf |= ITC_TRUECOLOR;
    else if (g_driverStates[DRVSTATE_TEX_COMPRESS] && !(flags & 0x200000))
        tf |= ITC_COMPRESS;
    if ((flags & 0x800000) && g_driverStates[DRVSTATE_TEX_MIPMAP])
        tf |= ITC_MIPMAP;
    if (g_driverStates[DRVSTATE_TEX_HALF] && !(flags & 0x400000))
        tf |= ITC_HALF;
    if (flags & 0x8000)
        tf |= ITC_KEEP_PIXELS;
    if (flags & MATF_COLORKEY)
        tf |= flags & 0x1000000 ? (ITC_ALPHA_1BIT | ITC_SOURCE_ALPHA) : ITC_ALPHA_1BIT;
    else if (flags & MATF_ALPHA)
        tf |= flags & 0x1000000 ? ITC_SOURCE_ALPHA : ITC_USE_ALPHA;
    char texName[256], alphaName[256];
    int length = 0;
    dtaRead(g_loadFile, &length, 1);
    if (!length)
        return;
    dtaRead(g_loadFile, texName, length);
    texName[length] = 0;
    alphaName[0] = 0;
    if ((flags & MATF_ALPHA) && !(flags & 0xa1000000)) {
        length = 0;
        dtaRead(g_loadFile, &length, 1);
        if (length)
            dtaRead(g_loadFile, alphaName, length);
        else
            tf &= ~ITC_USE_ALPHA;
        alphaName[length] = 0;
    }
    g_pIGraph->LoadTexture(texName, alphaName, tf, &texture);
}

void I3D_emitor::UpdateParticleBound()
{
    ClearParentBounds();
    I3D_bound_volume v{};
    v.bbox.min = S_vector(1e16f, 1e16f, 1e16f);
    v.bbox.max = S_vector(-1e16f, -1e16f, -1e16f);
    const S_matrix *inv;
    if (visualFlags & VISF_WORLD_SPACE) {
        inv = &kIdentityMatrix;
    } else {
        if (!(visualFlags & VISF_INVMATRIX_VALID)) {
            visualFlags |= VISF_INVMATRIX_VALID;
            invWorldMatrix.Inverse(GetMatrix());
        }
        inv = &invWorldMatrix;
    }
    for (int i = active.Size(); i != 0;) {
        const S_particle &p = particles[active[--i]];
        const S_vector lp = AsVector(p.position) * *inv;
        // The quad's corners lie within the cube around its center.
        const float r = p.scale * 0.8660255f;
        const S_vector lo(lp.x - r, lp.y - r, lp.z - r);
        if (lo.x < v.bbox.min.x)
            v.bbox.min.x = lo.x;
        if (lo.y < v.bbox.min.y)
            v.bbox.min.y = lo.y;
        if (lo.z < v.bbox.min.z)
            v.bbox.min.z = lo.z;
        const S_vector hi(lp.x + r, lp.y + r, lp.z + r);
        if (hi.x > v.bbox.max.x)
            v.bbox.max.x = hi.x;
        if (hi.y > v.bbox.max.y)
            v.bbox.max.y = hi.y;
        if (hi.z > v.bbox.max.z)
            v.bbox.max.z = hi.z;
    }
    // (Without particles the sphere spans the empty box's 1e16 corners.)
    const float hx = (v.bbox.max.x - v.bbox.min.x) * 0.5f;
    const float hy = (v.bbox.max.y - v.bbox.min.y) * 0.5f;
    const float hz = (v.bbox.max.z - v.bbox.min.z) * 0.5f;
    v.center = S_vector(hx + v.bbox.min.x, hy + v.bbox.min.y, hz + v.bbox.min.z);
    v.radius = std::sqrt((hz * hz + hy * hy) + hx * hx);
    SetBoundVolume(v);
}

S_particleVertex *I3D_emitor::LockVertices(int numQuads, float sortDepth)
{
    if (userVertices) {
        userQuads = numQuads;
        userDepth = sortDepth;
    }
    if (!numQuads)
        return nullptr;
    gfx::VertexBuffer *vb = s_vbs[s_vbIndex].vb;
    if (s_lockedVertices && s_writeQuad + numQuads < kMaxQuads) {
        firstQuad = s_writeQuad;
    } else if (s_writeQuad == kMaxQuads) {
        // The buffer's first lock this frame.
        if (numQuads >= kMaxQuads) {
            dbgPrintf("ERROR !! - I3D_emitor::LockVRAMVBuffer() - Allocate more memory for global particle VB !! %d", numQuads);
            return nullptr;
        }
        if (!vb)                            // (fix: no buffers, InitParticleBuffers failed)
            return nullptr;
        g_pGfx->Lock(vb, 0, 0, &s_lockedVertices, 0);
        firstQuad = 0;
        s_writeQuad = numQuads;
        return reinterpret_cast<S_particleVertex *>(s_lockedVertices);
    } else {
        if (s_writeQuad + numQuads >= kMaxQuads) {
            dbgPrintf("ERROR !! - I3D_emitor::LockVRAMVBuffer() - Allocate more memory for global particle VB !!");
            return nullptr;
        }
        if (!vb)
            return nullptr;
        g_pGfx->Lock(vb, 0, 0, &s_lockedVertices, 0);
        firstQuad = s_writeQuad;
    }
    s_writeQuad += numQuads;
    return reinterpret_cast<S_particleVertex *>(s_lockedVertices + firstQuad * 4 * sizeof(S_particleVertex));
}

void I3D_emitor::Prepare()
{
    if (visualFlags & VISF_MIRRORED)
        return;
    if (userVertices) {
        if (!userQuads)
            return;
        if (firstQuad < 0) {
            dbgPrintf("ERROR !! I3D_emitor::m_AddFGroups() cann't be called without previous I3D_emitor::m_LockVRAMVBuffer() call !!");
            return;
        }
        S_renderItem &item = QueueItem(this);
        const float key = (userDepth + 10000.0f) * 1000.0f;
        item.sortKey |= static_cast<uint32_t>(Fistp(key)) & RIKEY_DEPTH_MASK;
        return;
    }
    const int n = active.Size();
    if (!n)
        return;
    const int itemIndex = g_rc.items.Size();
    QueueItem(this);
    // The slots in drawing order: back to front, else the active list's from the end.
    const uint8_t *slot;
    int stride;
    uint32_t key;
    bool sorted = false;
    if (emitFlags & EMITF_SORTED) {
        s_keys.count = 0;
        if (static_cast<uint32_t>(n) > s_keys.capacity) {
            ::operator delete(s_keys.keys);
            s_keys.keys = static_cast<S_sortKey *>(::operator new(sizeof(S_sortKey) * n, std::nothrow));
            if (s_keys.keys)
                s_keys.capacity = static_cast<uint32_t>(n);
        }
        const S_matrix &view = g_renderCamera->viewMatrix;
        float sum = 0.0f;
        for (int i = n; i != 0;) {
            const int s = active[--i];
            const S_vector4 &pos = particles[s].position;
            const float z = ((pos.z * view.m[2][2] + pos.y * view.m[1][2]) + view.m[0][2] * pos.x) + view.m[3][2];
            sum = z + sum;
            if (s_keys.count == s_keys.capacity && !s_keys.Grow(s_keys.capacity * 2))
                continue;
            s_keys.keys[s_keys.count].key = static_cast<uint32_t>(Ftol(z * 1000.0f));
            s_keys.keys[s_keys.count].index = s;
            ++s_keys.count;
        }
        RadixSort(s_keys.keys, static_cast<int>(s_keys.count));
        key = static_cast<uint32_t>(Fistp((sum / static_cast<float>(n) + 10000.0f) * 1000.0f));
        // (Fix: out of memory some keys are missing, which the original reads anyway; the port
        // then keeps the unsorted order.)
        sorted = s_keys.count == static_cast<uint32_t>(n);
    } else {
        key = static_cast<uint32_t>(Fistp(depth));
    }
    if (sorted) {
        slot = reinterpret_cast<const uint8_t *>(&s_keys.keys[n - 1].index);
        stride = sizeof(S_sortKey);
    } else {
        slot = reinterpret_cast<const uint8_t *>(&active[n - 1]);
        stride = sizeof(int);
    }
    g_rc.items[itemIndex].sortKey |= key & RIKEY_DEPTH_MASK;
    const S_matrix &cam = g_renderCamera->GetMatrix();
    S_particleVertex *v = LockVertices(n, 0.0f);
    if (!v) {
        // (The item goes; the blended count stays, as in the original.)
        g_rc.items.PopBack();
        return;
    }
    for (int k = n; k != 0; --k) {
        const S_particle &p = particles[*reinterpret_cast<const int *>(slot)];
        slot -= stride;
        const uint32_t a = static_cast<uint32_t>(Fistp(p.alpha * 255.0f));
        const uint32_t r = static_cast<uint32_t>(Fistp(p.color.x * 255.0f));
        const uint32_t g = static_cast<uint32_t>(Fistp(p.color.y * 255.0f));
        const uint32_t b = static_cast<uint32_t>(Fistp(p.color.z * 255.0f));
        const uint32_t color = (((a << 8 | r) << 8 | g) << 8) | b;
        // The quad in the camera's plane, turned by the particle's spin, scaled.
        const float angle = static_cast<float>(static_cast<int>(p.age)) * p.rotationSpeed;
        S_matrix q;
        const S_matrix *axes = &cam;
        S_matrix spun;
        if (!IsTinyBits(std::fabs(angle))) {
            spun = RotateZMatrix(angle).RotateByMatrix(cam);
            axes = &spun;
        }
        for (int row = 0; row < 3; ++row) {
            q.m[row][0] = axes->m[row][0] * p.scale;
            q.m[row][1] = axes->m[row][1] * p.scale;
            q.m[row][2] = axes->m[row][2] * p.scale;
            q.m[row][3] = 0.0f;
        }
        q.m[3][0] = p.position.x;
        q.m[3][1] = p.position.y;
        q.m[3][2] = p.position.z;
        q.m[3][3] = p.position.w;
        // Its group's frame at its age.
        const S_vector4 *uv = &kWholeTexture;
        // (Fix: the original takes any group index below numGroups, negative ones too, and any
        // frame index the group's numbers give: a zero frame time divides by zero, a loop frame
        // past the frames reads past them.)
        if (numGroups && numGroups > p.group && p.group >= 0) {
            const S_emitorGroup &grp = groups[p.group];
            const int age = static_cast<int>(p.age);
            int frame = -1;
            if (grp.frameTime > 0) {
                if (age < grp.loopStart)
                    frame = age / grp.frameTime;
                else if (grp.loopLength > 0)
                    frame = (age - grp.loopStart) % grp.loopLength / grp.frameTime + grp.loopFrame;
            }
            if (frame >= 0 && frame < grp.frames.Size())
                uv = &grp.frames[frame];
        }
        auto corner = [&](float x, float y, float u, float tv) {
            const S_vector c = S_vector(x, y, 0.0f) * q;
            v->x = c.x;
            v->y = c.y;
            v->z = c.z;
            v->color = color;
            v->u = u;
            v->v = tv;
            ++v;
        };
        corner(-0.5f, -0.5f, uv->x, uv->w);
        corner(-0.5f, 0.5f, uv->x, uv->y);
        corner(0.5f, -0.5f, uv->z, uv->w);
        corner(0.5f, 0.5f, uv->z, uv->y);
    }
}

void I3D_emitor::Draw(const S_renderItem *item)
{
    int first;
    int quads;
    if (visualFlags & VISF_MIRRORED) {
        // A mirror's copy draws the quads its original wrote.
        auto *original = static_cast<I3D_emitor *>(data);
        first = original->firstQuad * 4;
        quads = userVertices ? original->userQuads : original->active.Size();
    } else {
        first = firstQuad * 4;
        firstQuad = -1;
        quads = userVertices ? userQuads : active.Size();
    }
    const int numVertices = quads * 4;
    if (emitFlags & EMITF_OPAQUE) {
        SetAlphaState(0);
    } else if (emitFlags & EMITF_BLEND) {
        SetAlphaState(1);
        SetBlendState(0);
    } else if (emitFlags & EMITF_CUSTOM_BLEND) {
        SetAlphaState(1);
        g_pGfx->SetRenderState(D3DRS_SRCBLEND, srcBlend);
        g_pGfx->SetRenderState(D3DRS_DESTBLEND, destBlend);
    } else {
        SetAlphaState(1);
        SetBlendState(1);
    }
    g_pGfx->SetTexture(0, item->texture ? item->texture->deviceTexture : nullptr);
    g_pGfx->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX *>(&kIdentityMatrix));
    SetColorOp(0, D3DTOP_MODULATE2X);       // 0x1003ec20
    SetAlphaOp(0, D3DTOP_MODULATE);         // 0x1003ec80
    SetColorOp(1, D3DTOP_DISABLE);          // 0x1003ec50
    // (Set past IGraph's address mode cache.)
    g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    g_pGfx->SetRenderState(D3DRS_LIGHTING, FALSE);
    const S_VBAlloc &vb = s_vbs[s_vbIndex];
    if (vb.buffer) {                        // (fix: no buffers, InitParticleBuffers failed)
        SetVertexFormat(static_cast<int>(vb.buffer->format), false);
        g_pGfx->SetIndices(s_ib.ib, vb.offset + first);
        g_pGfx->SetStreamSource(0, vb.vb, vb.buffer->stride);
        g_pGfx->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, numVertices, s_ib.offset, numVertices >> 1);
    }
    g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_pGfx->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    g_pGfx->SetRenderState(D3DRS_LIGHTING, TRUE);
    if (emitFlags & EMITF_CUSTOM_BLEND) {
        g_rs.blend = 0;
        g_pGfx->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pGfx->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
}
