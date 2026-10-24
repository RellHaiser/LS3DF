// The model loader (load.cpp, 0x1005d5b0 - 0x1005edc0): state the frames' Load methods share.
#pragma once

#include <windows.h>

#include "core/c_vector.h"

class I3D_frame;
class I3D_material;
class I3D_mesh;
class I3D_target;

namespace ls3d::i3d {

// The file being loaded (0x100d00f0) and the first failure (0x100d00d0).
extern int g_loadFile;
extern HRESULT g_loadResult;
// Targets whose frame indices the loader turns into frames at the end (0x100d0110).
extern C_vector<I3D_target *> g_loadTargets;
// The file's name (0x100d00f8, for the messages), its materials (0x100ca2c8, at most 6000; the
// files' material IDs count from 1) and the frames read so far (0x100c0680; frame references
// count from 1).
extern const char *g_loadFileName;
extern I3D_material *g_loadMaterials[6000];
extern int g_loadNumMaterials;              // 0x100d0088
extern I3D_frame *g_loadFrames[10000];
extern int g_loadNumFrames;                 // 0x100ca2c0

// A mesh from the file being loaded (0x1005ddd0; a new reference, null on failure).
I3D_mesh *LoadMesh();

}  // namespace ls3d::i3d
