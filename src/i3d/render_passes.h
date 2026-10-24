// The render passes Render runs, implemented with their modules.
#pragma once

class I3D_frame;
class I3D_mirror;
class I3D_scene;
class I3D_shadow;
class I3D_visual;

namespace ls3d::i3d {

// The sectors seen from the camera through the portals (portal_render.cpp, 0x10042fc0): each
// visible sector's visible visuals chained in renderVisuals with this frame's renderStamp.
void TraverseSectors(I3D_scene *scene);

// Shadows (shadow.cpp): each of the scene's before the sector pass (0x10050590), the queued
// ones' geometry built after the visuals (0x10050cc0; driver state DRVSTATE_SHADOWS).
void PrepareShadow(I3D_shadow *shadow);
void RenderShadows();

// Projectors (projector.cpp, 0x10044190; driver state DRVSTATE_PROJECTORS).
void RenderProjectors();

// Particles (emitter.cpp): the batched particles written out before a queue is drawn
// (0x1003ae10) and after the frame (0x1003ae40).
void FlushParticles();
// The emitters' shared particle buffers released (0x1003add0).
void ReleaseParticleBuffers();
// The skinned visuals' shared buffers freed (0x10053b90, singlemesh.cpp).
void FreeSkinBuffers();
// The projectors' shared geometry buffers freed (0x10001470, projector.cpp).
void FreeProjectionBuffers();
void EndParticles();

// Mirrors (mirror.cpp): visibility and the reflection's setup (0x10032e50), the reflected pass
// begun (0x10033410: false when nothing is reflected), the mirror's surface drawn into the z
// buffer (0x10032a60).
void MirrorPrepare(I3D_mirror *mirror);
bool MirrorBeginReflection(I3D_mirror *mirror);
void MirrorDrawSurface(I3D_mirror *mirror);

// A visual queued for the current pass (0x100632a0; render.cpp): its model's renderStamp set;
// projectors only with DRVSTATE_PROJECTORS; lens flares join the scene's list.
void RenderVisual(I3D_visual *visual);

}  // namespace ls3d::i3d
