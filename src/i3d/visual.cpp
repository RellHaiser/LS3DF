// I3D_visual (0x10056f80 - 0x10057660).
#include "i3d/visual.h"

#include <cstring>

namespace {

// The visual types by index (0x100a6edc), each fourCC a dword of the string.
const char kVisualFourCCs[] = "OBJ_LMAPSNGMSMMRBBRDMRPHLENSPROJMIRREMTRLNDPSHD";

}  // namespace

uint32_t VisualFourCC(int index)
{
    uint32_t fourCC;
    std::memcpy(&fourCC, kVisualFourCCs + index * 4, sizeof fourCC);
    return fourCC;
}

int VisualIndex(uint32_t fourCC)
{
    for (int i = 0; i < kVisualTypeCount; ++i) {
        if (VisualFourCC(i) == fourCC)
            return i;
    }
    return -1;
}

uint32_t __stdcall I3D_visual::GetVisualType() { return VisualFourCC(visualIndex); }

void I3D_visual::Init()
{
    I3D_frame::Init();
    invWorldMatrix.Identity();
    localBound.bbox.Invalidate();
    localBound.center.Zero();
    localBound.radius = 0.0f;
    worldBound.bbox.Invalidate();
    worldBound.center.Zero();
    worldBound.radius = 0.0f;
    visualFlags = VISF_MODE_A1 | VISF_MODE_B1;
    nextRenderVisual = nullptr;
}

void I3D_visual::MatrixChanged()
{
    if (!(visualFlags & VISF_WORLD_SPACE) && (frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED)))
        visualFlags &= ~(VISF_WBOUND_VALID | VISF_INVMATRIX_VALID);
}

void __stdcall I3D_visual::SetBoundVolume(const I3D_bound_volume &volume)
{
    localBound = volume;
    if (!(visualFlags & VISF_WORLD_SPACE))
        visualFlags &= ~VISF_WBOUND_VALID;
    frameFlags &= ~FRMFLAGS_BOUND_VALID;
}

void __stdcall I3D_visual::SetBBox(const I3D_bbox &box)
{
    localBound.bbox = box;
    if (!(visualFlags & VISF_WORLD_SPACE))
        visualFlags &= ~VISF_WBOUND_VALID;
    frameFlags &= ~FRMFLAGS_BOUND_VALID;
    if (!box.IsValid()) {
        localBound.center.Zero();
        localBound.radius = 0.0f;
        return;
    }
    localBound.MakeSphere();
}

void __stdcall I3D_visual::SetModeA(int mode)
{
    if (mode == 0)
        visualFlags &= ~(VISF_MODE_A1 | VISF_MODE_A2);
    else if (mode == 1)
        visualFlags = (visualFlags & ~VISF_MODE_A2) | VISF_MODE_A1;
    else if (mode == 2)
        visualFlags |= VISF_MODE_A1 | VISF_MODE_A2;
}

int __stdcall I3D_visual::GetModeA()
{
    if (visualFlags & VISF_MODE_A2)
        return 2;
    return (visualFlags & VISF_MODE_A1) ? 1 : 0;
}

void __stdcall I3D_visual::SetModeB(int mode)
{
    if (mode == 0)
        visualFlags &= ~(VISF_MODE_B1 | VISF_MODE_B2);
    else if (mode == 1)
        visualFlags = (visualFlags & ~VISF_MODE_B2) | VISF_MODE_B1;
    else if (mode == 2)
        visualFlags |= VISF_MODE_B1 | VISF_MODE_B2;
}

int __stdcall I3D_visual::GetModeB()
{
    if (visualFlags & VISF_MODE_B2)
        return 2;
    return (visualFlags & VISF_MODE_B1) ? 1 : 0;
}

void __stdcall I3D_visual::UpdateLocalBound() { visualFlags |= VISF_BOUND_VALID; }

void __stdcall I3D_visual::UpdateWBoundProc()
{
    const uint32_t old = visualFlags;
    visualFlags |= VISF_WBOUND_VALID;
    if (!(old & VISF_BOUND_VALID))
        UpdateLocalBound();
    const I3D_bbox &box = localBound.bbox;
    if (!box.IsValid()) {
        worldBound.center.Zero();
        worldBound.radius = 0.0f;
        worldBound.bbox.Invalidate();
        return;
    }
    const S_matrix &m = GetMatrix();
    worldBound.bbox.min = worldBound.bbox.max = box.Corner(0) * m;
    for (int i = 1; i < 8; ++i)
        worldBound.bbox.Expand(box.Corner(i) * m);
    worldBound.MakeSphere();
}
