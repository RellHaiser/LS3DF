// I3D_driver's frame services (0x1001a0f0 - 0x1001b400): the name tree of the named frames,
// hierarchy-wide parameters and the hierarchy boxes.
#include <cstring>
#include <new>

#include "core/debug.h"
#include "i3d/driver.h"
#include "i3d/frame.h"
#include "i3d/i3d_hooks.h"
#include "i3d/sector.h"
#include "i3d/visual.h"

using namespace ls3d;
using namespace ls3d::i3d;

namespace ls3d::i3d {

I3D_frame *g_nameRoot;                              // 0x100be010
I3D_frame *g_nameParent;                            // 0x100aded0

}  // namespace ls3d::i3d

namespace {

// RebalanceNameTree's work array of the frames in name order (0x100be014, 0x100be050,
// 0x100be16c).
I3D_frame **g_nameArray;
int g_nameArraySize;
int g_nameArrayCount;

// The value SetHierarchyLightLevel / SetHierarchyAlpha pass down (0x100be02c).
float g_hierarchyValue;
// GetHierarchyBBox's box (0x100be3e0).
I3D_bbox g_hierarchyBox;

const char *NameOf(const I3D_frame *frame) { return frame->name ? frame->name : ""; }

// The nodes of a name subtree (0x1001d240, 0x1001d2c0).
int CountNames(const I3D_frame *node)
{
    int n = 0;
    for (; node; node = node->nameRight)
        n += 1 + CountNames(node->nameLeft);
    return n;
}

// Appends a subtree to g_nameArray in name order (0x1001d2f0).
void CollectNames(I3D_frame *node)
{
    do {
        if (node->nameLeft)
            CollectNames(node->nameLeft);
        g_nameArray[g_nameArrayCount++] = node;
        node = node->nameRight;
    } while (node);
}

// Makes a balanced tree of g_nameArray[first .. first + count) under *link (0x1001a390). Equal
// names stay one chain to the right, the first of them the node.
void BuildNameTree(I3D_frame **link, int first, int count)
{
    while (count) {
        int mid = count / 2;
        while (mid > 0 && !std::strcmp(NameOf(g_nameArray[first + mid - 1]), NameOf(g_nameArray[first + mid])))
            --mid;
        I3D_frame *node = g_nameArray[first + mid];
        *link = node;
        I3D_frame *tail = node;
        int run = 1;
        for (int j = mid + 1; j < count && !std::strcmp(NameOf(node), NameOf(g_nameArray[first + j])); ++j) {
            tail->nameLeft = nullptr;
            tail->nameRight = g_nameArray[first + j];
            tail = tail->nameRight;
            ++run;
        }
        // Fix: the original leaves the left link of a chain's last frame as it was in the old
        // tree, which can make frames reachable twice (or loop).
        if (tail != node)
            tail->nameLeft = nullptr;
        if (mid > 0)
            BuildNameTree(&node->nameLeft, first, mid);
        else
            node->nameLeft = nullptr;
        if (run + mid >= count) {
            tail->nameRight = nullptr;
            return;
        }
        link = &tail->nameRight;
        first += run + mid;
        count -= run + mid;
    }
}

// The frames of a name subtree in name order until `proc` returns nonzero (0x1001a620).
bool EnumNameNodes(I3D_ENUMPROC *proc, void *context, I3D_frame *node)
{
    do {
        if (node->nameLeft && EnumNameNodes(proc, context, node->nameLeft))
            return true;
        if (proc(node, context))
            return true;
        node = node->nameRight;
    } while (node);
    return false;
}

void SetLightLevelProc(I3D_frame *frame)            // 0x1001a730
{
    if (frame->type == FRAME_VISUAL && (static_cast<I3D_visual *>(frame)->visualFlags & VISF_OBJECT_PARAMS))
        ObjectSetLightLevel(frame, g_hierarchyValue);
    for (I3D_frame *c = frame->firstChild; c; c = c->nextSibling)
        SetLightLevelProc(c);
}

void SetAlphaProc(I3D_frame *frame)                 // 0x1001a7e0
{
    if (frame->type == FRAME_VISUAL && (static_cast<I3D_visual *>(frame)->visualFlags & VISF_OBJECT_PARAMS))
        ObjectSetAlpha(frame, g_hierarchyValue);
    for (I3D_frame *c = frame->firstChild; c; c = c->nextSibling)
        SetAlphaProc(c);
}

// The box of a frame's visual and its descendants in the frame's space, each child's box
// transformed as a whole by the child's local matrix (0x1001aae0).
I3D_bbox HierarchyBound(I3D_frame *frame)
{
    I3D_bbox box;
    if (frame->type == FRAME_VISUAL)
        box = static_cast<I3D_visual *>(frame)->GetLocalBound().bbox;
    else
        box.Invalidate();
    for (I3D_frame *c = frame->firstChild; c; c = c->nextSibling) {
        const I3D_bbox childBox = HierarchyBound(c);
        if (!childBox.IsValid())
            continue;
        c->UpdateLMatrix();
        for (int i = 0; i < 8; ++i)
            box.Expand(childBox.Corner(i) * c->localMatrix);
    }
    return box;
}

// Adds the visual boxes of a hierarchy to g_hierarchyBox, `parentMatrix` taking the frame's parent
// to the space of the box (0x1001b120).
void AddVisualBoxes(I3D_frame *frame, const S_matrix &parentMatrix)
{
    frame->UpdateLMatrix();
    const S_matrix m = frame->localMatrix * parentMatrix;
    if (frame->type == FRAME_VISUAL) {
        const I3D_bbox &box = static_cast<I3D_visual *>(frame)->GetLocalBound().bbox;
        if (box.IsValid()) {
            for (int i = 0; i < 8; ++i)
                g_hierarchyBox.Expand(box.Corner(i) * m);
        }
    }
    for (I3D_frame *c = frame->firstChild; c; c = c->nextSibling)
        AddVisualBoxes(c, m);
}

}  // namespace

namespace ls3d::i3d {

I3D_frame *FindNameNode(const char *name, const I3D_frame *frame)
{
    g_nameParent = nullptr;
    I3D_frame *node = g_nameRoot;
    if (!node)
        return nullptr;
    for (;;) {
        const int c = std::strcmp(name, NameOf(node));
        if (c < 0) {
            if (!node->nameLeft)
                return nullptr;
            g_nameParent = node;
            node = node->nameLeft;
        } else if (c > 0) {
            if (!node->nameRight)
                return nullptr;
            g_nameParent = node;
            node = node->nameRight;
        } else {
            // The chain of equal names.
            for (;;) {
                if (node == frame)
                    return node;
                if (!node->nameRight) {
                    g_nameParent = nullptr;
                    return nullptr;
                }
                g_nameParent = node;
                I3D_frame *next = node->nameRight;
                if (std::strcmp(name, NameOf(next)))
                    return nullptr;
                node = next;
            }
        }
    }
}

}  // namespace ls3d::i3d

I3D_frame *__stdcall I3D_driver::FindFrame(I3D_scene *scene, const char *name)
{
    I3D_frame *node = g_nameRoot;
    while (node) {
        const int c = std::strcmp(name, NameOf(node));
        if (c < 0)
            node = node->nameLeft;
        else if (c > 0)
            node = node->nameRight;
        else
            break;
    }
    if (!node)
        return nullptr;
    // The first of the equal names in a sector of `scene`.
    for (;;) {
        I3D_frame *f = node;
        while (f && f->type != FRAME_SECTOR)
            f = f->parent;
        if (f && static_cast<I3D_sector *>(f)->scene == scene)
            return node;
        if (!scene)
            return node;
        I3D_frame *next = node->nameRight;
        if (!next || std::strcmp(name, NameOf(next)))
            return nullptr;
        node = next;
    }
}

void __stdcall I3D_driver::RebalanceNameTree()
{
    if (!g_nameRoot)
        return;
    g_nameArraySize = CountNames(g_nameRoot);
    g_nameArrayCount = 0;
    g_nameArray = static_cast<I3D_frame **>(::operator new(g_nameArraySize * sizeof(I3D_frame *), std::nothrow));
    if (!g_nameArray) {
        ReportError(I3DERR_OUTOFMEM, kSevFatal);
        return;
    }
    CollectNames(g_nameRoot);
    BuildNameTree(&g_nameRoot, 0, g_nameArrayCount);
    ::operator delete(g_nameArray);
}

I3D_RESULT __stdcall I3D_driver::EnumNamedFrames(I3D_ENUMPROC *proc, void *context)
{
    if (!g_nameRoot)
        return I3D_OK;
    return EnumNameNodes(proc, context, g_nameRoot) ? I3DERR_CANCELED : I3D_OK;
}

void __stdcall I3D_driver::FrameReleased(I3D_frame *frame)
{
    for (int i = scenes.Size(); i > 0; --i) {
        if (SceneRemoveSoundFrame(scenes[i - 1], frame) == I3D_OK)
            return;
    }
}

void __stdcall I3D_driver::RemoveShadowCaster(I3D_frame *frame) { ls3d::i3d::RemoveShadowCaster(frame); }
void __stdcall I3D_driver::RemoveTargetFrame(I3D_frame *frame) { RemoveFromTargets(frame); }

void __stdcall I3D_driver::SetHierarchyLightLevel(I3D_frame *frame, float level)
{
    g_hierarchyValue = level;
    SetLightLevelProc(frame);
}

void __stdcall I3D_driver::SetHierarchyAlpha(I3D_frame *frame, float alpha)
{
    g_hierarchyValue = alpha;
    SetAlphaProc(frame);
}

I3D_bbox *__stdcall I3D_driver::GetHierarchyBound(I3D_bbox &out, I3D_frame *frame)
{
    if (frame->type == FRAME_SECTOR)
        out = static_cast<I3D_sector *>(frame)->box;
    else
        out = HierarchyBound(frame);
    return &out;
}

I3D_bbox *__stdcall I3D_driver::GetHierarchyBBox(I3D_bbox &out, I3D_frame *frame)
{
    if (frame->type == FRAME_VISUAL)
        g_hierarchyBox = static_cast<I3D_visual *>(frame)->GetLocalBound().bbox;
    else
        g_hierarchyBox.Invalidate();
    for (I3D_frame *c = frame->firstChild; c; c = c->nextSibling)
        AddVisualBoxes(c, kIdentityMatrix);
    out = g_hierarchyBox;
    return &out;
}
