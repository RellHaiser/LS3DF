// I3D_camera (FRAME_CAMERA; vtable 0x1009bd70, 0x540 bytes; 0x10009840 - 0x1000c6a0).
//
// A camera keeps three projections: the main one (fov, near / far), the backdrop's (the scene's
// backdrop range, or a range 1000 times as deep) and the near sector's (first-person geometry:
// its own fov and range), each with its view x projection product and frustum planes in world
// space (plane 0 the view direction, then the four sides).
#pragma once

#include "i3d/frame.h"

class I3D_sector;

// A camera marks its frame with this flag (set by Init and Duplicate; read by the renderer).
enum : uint32_t {
    FRMFLAGS_CAMERA_PROJECTION = 0x08000000,
};

class I3D_camera : public I3D_frame {
public:
    // 0x4C 0x100099c0: also the projections of another camera.
    I3D_RESULT __stdcall Duplicate(I3D_frame *source) override;
    // 0x60 / 0x64 0x10009840 / 0x100099a0
    void Init() override;
    void Uninit() override;
    // 0x68 0x1000a200: the view matrices and the frustum planes after the camera moved.
    void MatrixChanged() override;
    // 0x74 / 0x78 0x10009e30 / 0x10009e90: field of view (radians, 0.01 .. pi) of the main / near
    // sector projection.
    virtual void __stdcall SetFOV(float angle);
    virtual void __stdcall SetNearSectorFOV(float angle);
    // 0x7C / 0x80 0x10009ef0 / 0x10009f50: clip range of the main / near sector projection (both
    // at least 0.001, 0.01 apart).
    virtual void __stdcall SetRange(float nearDist, float farDist);
    virtual void __stdcall SetNearSectorRange(float nearDist, float farDist);
    // 0x84 / 0x88 0x10009fb0 / 0x10009ff0: orthogonal projection: its scale, on / off.
    virtual void __stdcall SetOrthoScale(float scale);
    virtual void __stdcall SetOrthogonal(bool on);
    // 0x8C 0x1000a010: width / height (0.01 .. 100).
    virtual void __stdcall SetAspectRatio(float aspect);
    // 0x90 0x1000a0c0: whether a sphere (center, radius) is inside the main frustum.
    virtual bool __stdcall IsSphereVisible(const S_vector4 &sphere);

    // The projection matrices from the settings (0x10009b30).
    void BuildProjection();
    // The camera's sector updated from the scene (0x1000a060, before rendering).
    void UpdateSector();

    float fov;                              // +0x120
    float nearRange;                        // +0x124
    float farRange;                         // +0x128
    float nearSectorFov;                    // +0x12C
    float nearSectorNear;                   // +0x130
    float nearSectorFar;                    // +0x134
    float aspect;                           // +0x138
    bool ortho;                             // +0x13C
    uint8_t padding13D[3];
    float orthoScale;                       // +0x140
    uint32_t padding144[3];
    S_matrix viewMatrix;                    // +0x150 world -> view
    S_matrix projection;                    // +0x190 (4 x 4)
    S_matrix viewProjection;                // +0x1D0
    S_matrix invViewMatrix;                 // +0x210
    S_matrix viewRotation;                  // +0x250 viewMatrix without its translation
    S_matrix invViewRotation;               // +0x290
    S_matrix backdropProjection;            // +0x2D0
    S_matrix backdropViewProjection;        // +0x310 viewRotation x backdropProjection
    S_matrix nearSectorProjection;          // +0x350
    S_matrix nearSectorViewProjection;      // +0x390
    float nearHalfWidth;                    // +0x3D0 half the main frustum's width at the near plane
    float nearHalfHeight;                   // +0x3D4
    float nearSectorHalfWidth;              // +0x3D8 the same for the near sector projection
    float nearSectorHalfHeight;             // +0x3DC
    I3D_scene *scene;                       // +0x3E0 the scene the camera is active in
    I3D_sector *sector;                     // +0x3E4 the sector it is in
    int seenChangeCount;                    // +0x3E8 (renderer) changeCount at the last frame
    int seenSectorChangeCount;              // +0x3EC (renderer) the sector's changeCount then
    S_plane frustum[5];                     // +0x3F0 main frustum (plane 0's d: see frustumD)
    float frustumD[5];                      // +0x440 the planes' distances
    uint32_t padding454[3];
    S_plane backdropFrustum[5];             // +0x460
    float backdropFrustumD[5];              // +0x4B0
    uint32_t padding4C4[3];
    S_plane nearSectorFrustum[5];           // +0x4D0
    float nearSectorFrustumD[5];            // +0x520
    uint32_t padding534[3];
};
static_assert(sizeof(I3D_camera) == 0x540, "I3D_camera layout");
static_assert(offsetof(I3D_camera, viewMatrix) == 0x150, "I3D_camera layout");
static_assert(offsetof(I3D_camera, nearHalfWidth) == 0x3D0, "I3D_camera layout");
static_assert(offsetof(I3D_camera, frustum) == 0x3F0, "I3D_camera layout");
static_assert(offsetof(I3D_camera, nearSectorFrustumD) == 0x520, "I3D_camera layout");
