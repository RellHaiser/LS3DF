// I3D_billboard: an object visual that turns toward the camera (BBRD; vtable 0x1009c440, 0x260
// bytes; 0x10005f80 - 0x10007dd0). Its mesh is drawn with `matrix`, made each frame from the
// mode: turning about one of the frame's axes, or the camera's orientation.
#pragma once

#include "i3d/object.h"

// I3D_billboard::mode.
enum : int {
    BBRDMODE_AXIS_SPHERE = 0,               // about the axis, then tilted toward the camera
    BBRDMODE_AXIS = 1,                      // about the axis only
    BBRDMODE_SCREEN_SCALED = 2,             // the camera's orientation with the frame's scale
    BBRDMODE_SCREEN = 3,                    // the camera's orientation
    BBRDMODE_SCREEN_LOCAL = 4,              // the frame's rotation in camera space, grown with distance
};

class I3D_billboard : public I3D_object {
public:
    // 0x4C 0x100060d0: as I3D_object's, with the axis and mode of another billboard (without the
    // light level and alpha).
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    void Init() override;                                                      // 0x60 0x10005f80
    // 0x70 0x10007db0: the object, then the axis and the mode's low byte.
    void Load() override;
    // 0x90 0x10005fa0: the cube around the mesh's sphere (it turns).
    void __stdcall UpdateLocalBound() override;
    // 0x94 0x100078e0: as I3D_object's with the facing matrix (its render vertices made with it).
    void Prepare() override;
    // 0x9C 0x100060a0: the mesh, then the bound.
    I3D_RESULT __stdcall SetMesh(I3D_mesh *newMesh, uint32_t flags) override;

    // 0x10006230: the matrix facing `camera` (a world position; the modes 2 - 4 the render camera's
    // orientation) by the mode and axis.
    void ComputeMatrix(const S_vector &camera, S_matrix &out);

    int axis;                               // +0x210 (2) 0 x, 1 y, 2 z
    int mode;                               // +0x214 BBRDMODE_*
    uint32_t unknown218[2];                 // +0x218
    S_matrix matrix;                        // +0x220 the facing matrix drawn with
};
static_assert(sizeof(I3D_billboard) == 0x260, "I3D_billboard layout");
static_assert(offsetof(I3D_billboard, matrix) == 0x220, "I3D_billboard layout");
