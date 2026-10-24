// I3D_joint (0x10020a70 - 0x10020bc0).
#include "i3d/joint.h"

#include "external/rw_data.h"
#include "i3d/loader.h"
#include "i3d/singlemesh.h"

using namespace ls3d::i3d;

void I3D_joint::Init()
{
    I3D_frame::Init();
    jointFlags = 0;
    jointIndex = 0;
    mesh = nullptr;
}

void I3D_joint::Uninit()
{
    if (mesh) {
        if (mesh->joints && mesh->joints[jointIndex])
            mesh->joints[jointIndex]->mesh = nullptr;
        // Fix: the original also clears the slot when the mesh has no joint array (a null write).
        if (mesh->joints)
            mesh->joints[jointIndex] = nullptr;
    }
    I3D_frame::Uninit();
}

void I3D_joint::MatrixChanged()
{
    if (!(frameFlags & (FRMFLAGS_CHANGED | FRMFLAGS_CHILD_CHANGED)))
        return;
    jointFlags &= ~(JOINTF_8 | JOINTF_MODEL_MATRIX_VALID);
    if ((frameFlags & FRMFLAGS_CHANGED) && mesh) {
        ++mesh->jointChanges;
        mesh->visualFlags &= ~(VISF_BOUND_VALID | VISF_WBOUND_VALID);
        mesh->ClearParentBounds();
        mesh->frameFlags = (mesh->frameFlags & ~FRMFLAGS_BOUND_VALID) | FRMFLAGS_CHANGED;
    }
}

I3D_RESULT __stdcall I3D_joint::Duplicate(I3D_frame *source)
{
    if (!source)
        return I3DERR_INVALIDPARAMS;
    if (source->type == FRAME_JOINT) {
        jointFlags = 0;
        jointIndex = static_cast<I3D_joint *>(source)->jointIndex;
    }
    return I3D_frame::Duplicate(source);
}

void I3D_joint::Load()
{
    I3D_frame::Load();
    dtaRead(g_loadFile, &jointIndex, sizeof jointIndex);
}

const S_matrix &I3D_joint::GetModelMatrix()
{
    if (jointFlags & JOINTF_MODEL_MATRIX_VALID)
        return modelMatrix;
    jointFlags |= JOINTF_MODEL_MATRIX_VALID;
    UpdateLMatrix();
    modelMatrix = localMatrix;
    for (I3D_frame *p = parent; p; p = p->parent) {
        const uint16_t t = p->type;
        if (t == FRAME_VISUAL || t == FRAME_SECTOR || t == FRAME_MODEL)
            break;
        if (t == FRAME_JOINT && (static_cast<I3D_joint *>(p)->jointFlags & JOINTF_MODEL_MATRIX_VALID)) {
            modelMatrix = modelMatrix * static_cast<I3D_joint *>(p)->modelMatrix;
            break;
        }
        p->UpdateLMatrix();
        modelMatrix = modelMatrix * p->localMatrix;
    }
    return modelMatrix;
}
