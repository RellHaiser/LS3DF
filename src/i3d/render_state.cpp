// The renderer's shared state (render_state.h).
#include "i3d/render_state.h"

#include <new>

namespace ls3d::i3d {

S_renderContext g_rc;
I3D_camera *g_renderCamera;
I3D_scene *g_renderScene;
int g_renderStamp;
int g_prevRenderStamp;
float g_fovScale;
float g_fovScaleSq;
float g_lodScale;
const S_matrix *g_viewProjection;
float g_lensFlareScale;
bool g_rendering;
bool g_inRenderCallback;
bool g_cameraChanged;
bool g_forceCameraChanged = true;
bool g_mainPass;
C_vector<I3D_area *> g_activeAreas(0x20);
C_vector<I3D_sound *> g_areaSoundsLeaving(0x20);
S_alphaCacheEntry *g_alphaCache;
int g_alphaCacheSize;
int g_alphaCacheNext;
int g_alphaCompareMode;

// 0x1003eb90
bool S_renderContext::GrowKeys(int capacity)
{
    S_sortKey *p = static_cast<S_sortKey *>(::operator new(sizeof(S_sortKey) * capacity, std::nothrow));
    if (!p)
        return false;
    for (int i = 0; i < numKeys; ++i)
        p[i] = keys[i];
    keyCapacity = capacity;
    ::operator delete(keys);
    keys = p;
    return true;
}

void ResetRenderLists()
{
    g_rc.numOpaque = 0;
    g_rc.numBlended = 0;
    g_rc.items.Clear();
    g_rc.numKeys = 0;
    g_rc.items.Free();
}

}  // namespace ls3d::i3d
