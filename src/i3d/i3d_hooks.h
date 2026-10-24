// Calls of the frame code into frame types and services ported in their own modules. Each is the
// original's function at the address given (free functions there too, or the member noted).
#pragma once

#include "i3d/i3d.h"

class I3D_animation_set;
class I3D_frame;
class I3D_light;
class I3D_model;
class I3D_scene;
class I3D_sector;
struct S_shadowData;

namespace ls3d::i3d {

// Puts a frame whose last reference went on its type's free list after Uninit (0x10019dc0,
// driver_factory.cpp).
void FreeFrameObject(I3D_frame *frame);

// Targets (target.cpp): the targets' frame lists lose `frame` (0x100565f0); a target's frames
// follow it (0x100566d0). Targets moved by Update wait in g_targetUpdates (0x100be464) until the
// outermost Update ends (g_targetUpdateBusy 0x100be478).
void RemoveFromTargets(I3D_frame *frame);
void UpdateTarget(I3D_frame *target);

// Shadows (shadow.cpp): the shadows' caster lists lose `frame` (0x1004efd0); the frame a shadow
// is cast from (I3D_shadow 0x1004e970, __thiscall; null when its parent goes away).
void RemoveShadowCaster(I3D_frame *frame);
void ShadowSetSource(I3D_frame *shadow, I3D_frame *source);

// The loader (load.cpp): an animation file into the set's blob (0x1005d5b0, (file, set)); a model
// or scene file's frames below `root` (0x1005d260, __fastcall (file, flags) + callback, context,
// root: the model, or the scene's primary sector).
I3D_RESULT OpenAnimationFile(const char *fileName, I3D_animation_set *set);
I3D_RESULT OpenModelFile(const char *fileName, uint32_t flags, void *callback, void *context, I3D_frame *root);

// The driver's frame factory (I3D_driver::CreateFrame's worker, 0x10018e40): a new frame of the
// type (from its free list when there is one), initialized.
I3D_frame *CreateFrameObject(int type);
// The driver's visual factory (0x10019750, driver_core.cpp): a new visual of type `index`
// (visualIndex; from its free list when there is one), initialized.
I3D_frame *CreateVisualObject(int index);
// A new FRAME_SHADOW frame (I3D_shadow, shadow.cpp; not initialized).
I3D_frame *NewShadowFrame();
// A skinned visual's (SNGM / SMMR) vertices of mesh level `level` in world or local space as it
// draws them (I3D_singlemesh slot 0xBC).
const S_vector4 *SkinnedVertices(I3D_frame *visual, int level);
// The loader (load.cpp, 0x1005d740): a shadow file into `data` (`fileName`'s extension replaced
// by ".6ds"; waits up to 4 s for a load in progress).
I3D_RESULT OpenShadowFile(const char *fileName, uint32_t flags, S_shadowData *data);
// The portals of a scene's sectors connected again (0x10042940, portal_build.cpp).
void BuildPortals(I3D_scene *scene);

// A skinned visual's joints after its hierarchy was copied (I3D_singlemesh 0x100527d0).
void SkinFixJoints(I3D_frame *visual);


// The model / scene that loaded a frame keeps it / forgets it (I3D_model slots 0x7C / 0x80,
// I3D_scene slots 0x80 / 0x84).
void ModelAddFrame(I3D_frame *model, I3D_frame *frame);
void SceneAddFrame(I3D_frame *scene, I3D_frame *frame);
void ModelFrameReleased(I3D_frame *model, I3D_frame *frame);
void SceneFrameReleased(I3D_frame *scene, I3D_frame *frame);
// A scene's list of FRMFLAGS_SCENE_SOUND frames loses `frame` (I3D_scene slot 0xCC);
// I3DERR_OBJECTNOTFOUND when it is not there.
I3D_RESULT SceneRemoveSoundFrame(I3D_scene *scene, I3D_frame *frame);
// The sector of `scene` that holds `pos`, starting from `hint` (I3D_scene slot 0xDC).
I3D_sector *SceneLocateSector(I3D_scene *scene, const S_vector &pos, I3D_sector *hint);
// A scene's list of drawn dummies loses `dummy` (I3D_scene slot 0x12C).
void SceneRemoveDummy(I3D_scene *scene, I3D_frame *dummy);


// I3D_object (object.cpp) slots 0xA4 / 0xA8: the light level / alpha of an object visual.
void ObjectSetLightLevel(I3D_frame *visual, float level);
void ObjectSetAlpha(I3D_frame *visual, float alpha);

}  // namespace ls3d::i3d
