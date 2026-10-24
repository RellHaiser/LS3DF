// Parts of the I3D scene engine that IGraph and ISND call into (implemented with the I3D modules).
#pragma once

#include <cstdint>

namespace ls3d::i3d {

// I3D_driver::GetStats(0) block (0x100adeb0, 8 dwords), cleared by IGraph::Init.
extern uint32_t g_renderStats[8];

// After a device reset: forgets the driver's cached states and sets them again (each of the 0x45
// states at 0x100be058 set to -1, then I3D_driver::SetState(i, old)).
void ReapplyDriverStates();
// I3D_driver::DrawText2D(x, y, text, 0, 1.0f) (slot 0x40), used by the FPS overlay.
void DrawDebugText(int x, int y, const char *text);
// I3D_driver::DrawText2D (slot 0x40) with its flags and scale (the sound statistics).
void DrawText2D(int x, int y, const char *text, uint32_t flags, float scale);
// Driver state 4 (0x100be068): w-buffer instead of z.
bool UseWBuffer();
// Marks the hardware lights for re-sending after a device reset (0x100647f0).
void InvalidateLights();
// Re-creates the particle emitters' shared buffers (0x1003ac00).
int InitParticleBuffers();
// After a master volume change (ISND_driver::SetVolume): every sound frame of the driver's list
// (I3D_driver +0x08 / +0x10, from the last) that is open (I3D_sound slot 0x78 with 0) gets the
// volume-changed flag (8 at +0x120) and updates its sound (0x10055210).
void RefreshSoundVolumes();

}  // namespace ls3d::i3d
