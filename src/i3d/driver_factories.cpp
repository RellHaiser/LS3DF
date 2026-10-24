// I3D_driver's object factories (slots 0x58 - 0x78, 0x10018ae0 - 0x10018e40): materials,
// meshes and their parts, animation sets, poses.
#include "i3d/driver.h"

#include "core/debug.h"
#include "i3d/anim.h"
#include "i3d/material.h"
#include "i3d/mesh.h"

using namespace ls3d;

I3D_material *I3D_driver::CreateMaterial()
{
    auto *m = new I3D_material;
    if (!m)
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
    return m;
}

I3D_anim_material *I3D_driver::CreateAnimMaterial()
{
    auto *m = new I3D_anim_material;
    if (!m)
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
    return m;
}

I3D_triface *I3D_driver::AllocFaces(int count)
{
    return static_cast<I3D_triface *>(::operator new(sizeof(I3D_triface) * static_cast<size_t>(count)));
}

I3D_face_group *I3D_driver::CreateFaceGroup() { return new I3D_face_group; }

I3D_mesh_level *I3D_driver::CreateMeshLevel() { return new I3D_mesh_level; }

I3D_mesh *I3D_driver::CreateMesh() { return new I3D_mesh; }

I3D_animation_set *I3D_driver::CreateAnimationSet() { return new I3D_animation_set; }

I3D_pose *I3D_driver::CreatePose() { return new I3D_pose; }

I3D_pose_set *I3D_driver::CreatePoseSet() { return new I3D_pose_set; }
