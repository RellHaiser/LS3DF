// Differential test of the file loaders (src/i3d/load.cpp: OpenModelFile, ReadModelFile, LoadMesh,
// OpenAnimationFile, OpenShadowFile; I3D_model::Open, I3D_scene::Open, I3D_shadow::Open, every
// frame type's Load, the materials and their textures, the animation sets): the original
// LS3DF.dll and the port's built LS3DF.dll are loaded side by side in one process, each on its own
// fake Direct3D device (textures, vertex and index buffers in system memory), and both load the
// game's files from its .dta archives: every model (MODELS\*.4ds, every other one named so its
// frames get the prefix) and shadow file (*.6ds), every mission's scene (into a scene), map and
// loading screen, under five combinations of the driver's texture states in turn. Then files the
// test writes for what the game's lack (see WriteSyntheticModels): every other frame and visual
// type, built meshes, targets listing frames, corrupt files.
//
// After each file the two object graphs are walked together from the model / scene / shadow:
// every member of every object reached is compared (the layouts come from the port DLL's PDB
// through DbgHelp, so no member is left out), pointers are paired one to one (what is shared,
// linked or listed must match), each polymorphic object's class is matched by its vtable, and
// strings, arrays (by their counts: the rules below), vertex / index buffer bytes and texture
// levels are compared. Then both sides release it and must keep the same textures cached.
//
// Not failures: members the original leaves unset (heap or stack garbage: padding, unknownNNN
// words nothing writes, the w words of boxes and of some vectors, members only the renderer or
// the application sets) are compared apart and listed as "unset member differs" with how often
// the original's value was 0; caches are compared only while their valid flag is set. The run
// lists pointers it could not follow (none for the game's files) and pointers to plain structs
// it compared as one object.
//
// Usage: compare_loader.exe [original LS3DF.dll] [port LS3DF.dll] [first file] [count] [filter] [all]
//   Empty arguments take the defaults (the original as test_paths.h finds it, build\msvc\LS3DF.dll
//   with its PDB). The archives are mounted from the original's folder; the file names come from
//   the unpacked data (test_paths.h: UnpackedDir): MODELS, then MISSIONS\*\. `filter` keeps the
//   names containing it; `all` (triage) goes on after a value difference and lists every one.
//
// test-libs: dbghelp.lib /LARGEADDRESSAWARE
#include <windows.h>

#include <dbghelp.h>
#include <float.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/external/rw_data.h"
#include "../src/i3d/anim.h"
#include "../src/i3d/driver.h"
#include "../src/i3d/joint.h"
#include "../src/i3d/light.h"
#include "../src/i3d/material.h"
#include "../src/i3d/mesh.h"
#include "../src/i3d/mirror.h"
#include "../src/i3d/model.h"
#include "../src/i3d/morph.h"
#include "../src/i3d/occluder.h"
#include "../src/i3d/sector.h"
#include "../src/i3d/target.h"
#include "../src/i3d/portal.h"
#include "../src/i3d/singlemesh.h"
#include "../src/igraph/igraph.h"
#include "../src/igraph/itexture.h"
#include "fake_d3d8.h"
#include "test_paths.h"

namespace {

using namespace ls3d;

template <class R, class... A> R CallV(const void *object, int slot, A... args)
{
    using F = R(__stdcall *)(const void *, A...);
    return (*reinterpret_cast<F *const *>(object))[slot / 4](object, args...);
}
template <class T> T &Field(void *object, size_t offset) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset); }

// ---------------------------------------------------------------- the fake devices

struct FakeVB : FakeDirect3DVertexBuffer8Base {
    D3DVERTEXBUFFER_DESC desc = {};
    std::vector<uint8_t> data;
    HRESULT __stdcall Lock(UINT offset, UINT, BYTE **out, DWORD) override
    {
        *out = data.data() + offset;
        return S_OK;
    }
    HRESULT __stdcall Unlock() override { return S_OK; }
    HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC *out) override
    {
        *out = desc;
        return S_OK;
    }
    // (The object stays, for its identity; its bytes go.)
    void OnFinalRelease() override { std::vector<uint8_t>().swap(data); }
};
struct FakeIB : FakeDirect3DIndexBuffer8Base {
    D3DINDEXBUFFER_DESC desc = {};
    std::vector<uint8_t> data;
    HRESULT __stdcall Lock(UINT offset, UINT, BYTE **out, DWORD) override
    {
        *out = data.data() + offset;
        return S_OK;
    }
    HRESULT __stdcall Unlock() override { return S_OK; }
    HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC *out) override
    {
        *out = desc;
        return S_OK;
    }
    void OnFinalRelease() override { std::vector<uint8_t>().swap(data); }
};

struct LoaderDevice : FakeDevice {
    HRESULT __stdcall ResourceManagerDiscardBytes(DWORD) override { return S_OK; }
    HRESULT __stdcall CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                         IDirect3DVertexBuffer8 **out) override
    {
        auto *b = new FakeVB;
        b->desc.Format = D3DFMT_VERTEXDATA;
        b->desc.Type = D3DRTYPE_VERTEXBUFFER;
        b->desc.Usage = usage;
        b->desc.Pool = pool;
        b->desc.Size = length;
        b->desc.FVF = fvf;
        b->data.resize(length + 0x10000);       // (slack: some fills write past their allocation)
        *out = b;
        return S_OK;
    }
    HRESULT __stdcall CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                        IDirect3DIndexBuffer8 **out) override
    {
        auto *b = new FakeIB;
        b->desc.Format = format;
        b->desc.Type = D3DRTYPE_INDEXBUFFER;
        b->desc.Usage = usage;
        b->desc.Pool = pool;
        b->desc.Size = length;
        b->data.resize(length + 0x10000);
        *out = b;
        return S_OK;
    }
};

// ---------------------------------------------------------------- the two DLLs

struct Side {
    const char *label = "";
    HMODULE dll = nullptr;
    I3D_driver *driver = nullptr;
    LoaderDevice device;
};
Side g_side[2];

HANDLE g_proc;
DWORD64 g_portBase;

uint8_t *OrigVa(uint32_t va) { return reinterpret_cast<uint8_t *>(g_side[0].dll) + (va - 0x10000000); }
uint32_t ToOrigVa(uintptr_t a) { return static_cast<uint32_t>(a - reinterpret_cast<uintptr_t>(g_side[0].dll) + 0x10000000); }

struct SymbolBuffer {
    alignas(SYMBOL_INFO) char bytes[sizeof(SYMBOL_INFO) + 1024] = {};
    SYMBOL_INFO *operator->() { return reinterpret_cast<SYMBOL_INFO *>(bytes); }
    SYMBOL_INFO *get() { return reinterpret_cast<SYMBOL_INFO *>(bytes); }
    SymbolBuffer()
    {
        get()->SizeOfStruct = sizeof(SYMBOL_INFO);
        get()->MaxNameLen = 1024;
    }
};

uint8_t *PortSymbol(const char *name)
{
    SymbolBuffer s;
    const std::string q = std::string("port!") + name;
    if (!SymFromName(g_proc, q.c_str(), s.get())) {
        std::printf("the port's symbol %s is not in its PDB (%lu)\n", name, GetLastError());
        std::exit(2);
    }
    return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(s->Address));
}

// The port's symbol starting exactly at `address` ("" if none).
std::string PortSymbolAt(uintptr_t address)
{
    SymbolBuffer s;
    DWORD64 displacement = 0;
    if (!SymFromAddr(g_proc, address, &displacement, s.get()) || displacement)
        return "";
    return s->Name;
}

// A global of each side: the original's address, the port's name.
struct S_global {
    uint32_t va;
    const char *name;
};
template <class T> T &Global(int side, const S_global &g)
{
    return *reinterpret_cast<T *>(side ? PortSymbol(g.name) : OrigVa(g.va));
}
const S_global kLogOff = {0x100d16f2, "ls3d::g_logDisabled"};
const S_global kDevice = {0x100d216c, "ls3d::ig::g_pDevice"};
const S_global kFmt16 = {0x100d1da8, "ls3d::ig::g_fmt16Bit"};
const S_global kFmt1 = {0x100d1a74, "ls3d::ig::g_fmt1BitAlpha"};
const S_global kFmtAlpha = {0x100d1a78, "ls3d::ig::g_fmtAlpha"};
const S_global kFmtTrue = {0x100d1774, "ls3d::ig::g_fmtTrueColor"};
const S_global kFmtDxt1 = {0x100d1898, "ls3d::ig::g_fmtDxt1"};
const S_global kFmtDxt3 = {0x100d1edc, "ls3d::ig::g_fmtDxt3"};
const S_global kFmtDxt5 = {0x100d1a90, "ls3d::ig::g_fmtDxt5"};
const S_global kFmtBump = {0x100d1d10, "ls3d::ig::g_fmtBump"};
const S_global kCaps = {0x100d1dac, "ls3d::ig::g_caps"};
const S_global kMaxW = {0x100d2128, "ls3d::ig::g_maxTextureWidth"};
const S_global kMaxH = {0x100d1ce8, "ls3d::ig::g_maxTextureHeight"};
const S_global kCachedTextures = {0x100d19dc, "ls3d::ig::g_cachedTextures"};     // C_vector (count at +8)
const S_global kUncachedTextures = {0x100d19cc, "ls3d::ig::g_uncachedTextures"};

// ---------------------------------------------------------------- types from the port's PDB

enum : DWORD {
    kTagData = 7, kTagUDT = 11, kTagEnum = 12, kTagFunctionType = 13, kTagPointer = 14, kTagArray = 15,
    kTagBase = 16, kTagTypedef = 17, kTagBaseClass = 18, kTagVTable = 25,
};
enum : DWORD { kBtVoid = 1, kBtChar = 2, kBtFloat = 8 };
constexpr DWORD kDataIsMember = 7;

template <class T> bool TI(ULONG id, IMAGEHLP_SYMBOL_TYPE_INFO what, T *out)
{
    return SymGetTypeInfo(g_proc, g_portBase, id, what, out) != FALSE;
}

struct S_field {
    std::string name, owner, key;           // key: "owner::name"
    uint32_t offset;
    uint32_t local;                         // in the class declaring it
    ULONG type;
    bool bitfield, overlapped;
};
struct S_type {
    DWORD tag = 0;
    ULONG id = 0;
    uint32_t size = 0;
    std::string name;                       // UDTs
    DWORD baseType = 0;
    ULONG target = 0;                       // pointers: the pointee; arrays: the element
    uint32_t count = 0;                     // arrays
    bool polymorphic = false;
    std::vector<S_field> fields;            // UDTs: every member, the bases' included, by offset
};
std::unordered_map<ULONG, S_type> g_types;

std::string SymName(ULONG id)
{
    WCHAR *w = nullptr;
    if (!TI(id, TI_GET_SYMNAME, &w) || !w)
        return "";
    char text[1024];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, text, sizeof text, nullptr, nullptr);
    LocalFree(w);
    return text;
}

ULONG TypeByName(const std::string &name)
{
    SymbolBuffer s;
    if (!SymGetTypeFromName(g_proc, g_portBase, name.c_str(), s.get()))
        return 0;
    return s->TypeIndex;
}

std::vector<ULONG> Children(ULONG id)
{
    DWORD n = 0;
    if (!TI(id, TI_GET_CHILDRENCOUNT, &n) || !n)
        return {};
    std::vector<uint8_t> buf(sizeof(TI_FINDCHILDREN_PARAMS) + n * sizeof(ULONG));
    auto *fc = reinterpret_cast<TI_FINDCHILDREN_PARAMS *>(buf.data());
    fc->Count = n;
    fc->Start = 0;
    if (!SymGetTypeInfo(g_proc, g_portBase, id, TI_FINDCHILDREN, fc))
        return {};
    return std::vector<ULONG>(fc->ChildId, fc->ChildId + n);
}

const S_type &GetType(ULONG id);

void Flatten(ULONG id, uint32_t base, const std::string &owner, S_type &t)
{
    for (ULONG c : Children(id)) {
        DWORD tag = 0;
        TI(c, TI_GET_SYMTAG, &tag);
        if (tag == kTagBaseClass) {
            DWORD off = 0, bt = 0;
            TI(c, TI_GET_OFFSET, &off);
            TI(c, TI_GET_TYPEID, &bt);
            const S_type &b = GetType(bt);
            for (S_field f : b.fields) {
                f.offset += base + off;
                t.fields.push_back(f);
            }
            t.polymorphic |= b.polymorphic;
        } else if (tag == kTagVTable) {
            t.polymorphic = true;
        } else if (tag == kTagData) {
            DWORD kind = 0;
            TI(c, TI_GET_DATAKIND, &kind);
            if (kind != kDataIsMember)
                continue;
            S_field f;
            f.name = SymName(c);
            f.owner = owner;
            f.key = owner + "::" + f.name;
            DWORD off = 0, ft = 0, bit = 0;
            TI(c, TI_GET_OFFSET, &off);
            TI(c, TI_GET_TYPEID, &ft);
            f.offset = base + off;
            f.local = off;
            f.type = ft;
            f.bitfield = TI(c, TI_GET_BITPOSITION, &bit);
            f.overlapped = false;
            t.fields.push_back(f);
        }
    }
}

const S_type &GetType(ULONG id)
{
    auto it = g_types.find(id);
    if (it != g_types.end())
        return it->second;
    S_type t;
    t.id = id;
    TI(id, TI_GET_SYMTAG, &t.tag);
    if (t.tag == kTagTypedef) {
        DWORD under = 0;
        TI(id, TI_GET_TYPEID, &under);
        const S_type u = GetType(under);
        return g_types.emplace(id, u).first->second;
    }
    ULONG64 length = 0;
    TI(id, TI_GET_LENGTH, &length);
    t.size = static_cast<uint32_t>(length);
    switch (t.tag) {
    case kTagBase:
        TI(id, TI_GET_BASETYPE, &t.baseType);
        break;
    case kTagPointer:
    case kTagArray: {
        DWORD target = 0, count = 0;
        TI(id, TI_GET_TYPEID, &target);
        t.target = target;
        if (t.tag == kTagArray) {
            TI(id, TI_GET_COUNT, &count);
            t.count = count;
        }
        break;
    }
    case kTagUDT: {
        t.name = SymName(id);
        if (!t.size || Children(id).empty()) {
            // A forward declaration: the definition's type.
            const ULONG full = TypeByName(t.name);
            if (full && full != id) {
                const S_type f = GetType(full);
                if (f.size)
                    return g_types.emplace(id, f).first->second;
            }
        }
        Flatten(id, 0, t.name, t);
        std::stable_sort(t.fields.begin(), t.fields.end(),
                         [](const S_field &a, const S_field &b) { return a.offset < b.offset; });
        for (size_t i = 0; i + 1 < t.fields.size(); ++i) {
            S_field &a = t.fields[i], &b = t.fields[i + 1];
            if (a.offset == b.offset && !a.bitfield && !b.bitfield)
                a.overlapped = b.overlapped = true;
        }
        // (No member at 0: the vtable pointer is there.)
        if (t.size >= 4 && (t.fields.empty() || t.fields[0].offset >= 4))
            t.polymorphic = true;
        break;
    }
    default:
        break;
    }
    return g_types.emplace(id, std::move(t)).first->second;
}

// ---------------------------------------------------------------- the walk

struct S_item {
    ULONG type;
    uint8_t *o, *p;
    int parent;
    std::string label;
};
std::vector<S_item> g_items;
std::deque<int> g_queue;
std::set<std::pair<uintptr_t, ULONG>> g_walked;
struct S_pair {
    uintptr_t other;
    int item;
};
std::unordered_map<uintptr_t, S_pair> g_o2p, g_p2o;
std::unordered_map<uintptr_t, uintptr_t> g_vtO2P, g_vtP2O;   // the classes (kept for the whole run)
std::unordered_map<uintptr_t, ULONG> g_vtType;
int g_current = -1;
std::string g_currentClass;                 // its class
std::vector<std::string> g_inline;          // the members inside the current item
std::string g_model;

struct S_mismatch {
    long count = 0;
    long originalZero = 0;                  // (unset members: how often the original's was 0)
    std::string example;
};
std::map<std::string, S_mismatch> g_mismatches;   // by member
std::map<std::string, long> g_unhandled;           // pointer members not compared
std::map<std::string, long> g_overlapped;          // union members not compared
std::map<std::string, long> g_classes;             // objects compared, by class
std::map<std::string, long> g_plainTargets;        // pointers to plain structs (one object or an array?)
std::map<int, long> g_formats;                     // the textures compared, by format
long g_targetFrames;                               // frames the targets compared hold
std::set<uintptr_t> g_uvAllocs;                    // texture levels' S_VBAlloc (original side)
bool g_verbose = true;

struct ModelFailure {};

std::string ItemPath(int item)
{
    std::vector<const std::string *> parts;
    for (int i = item; i >= 0; i = g_items[i].parent)
        parts.push_back(&g_items[i].label);
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty() && (**it)[0] != '[')
            out += '.';
        out += **it;
    }
    return out;
}

std::string Path()
{
    std::string out = g_current >= 0 ? ItemPath(g_current) : std::string("?");
    for (const std::string &s : g_inline) {
        if (s[0] != '[')
            out += '.';
        out += s;
    }
    return out;
}

// The member being compared and the object holding it.
struct S_ctx {
    const std::string *key;
    uint8_t *oObj, *pObj;
};

[[noreturn]] void Mismatch(const S_ctx &c, const std::string &what)
{
    S_mismatch &m = g_mismatches[*c.key];
    if (!m.count++) {
        m.example = g_model + ": " + Path() + " (" + g_currentClass + "): " + what;
        if (g_verbose)
            std::printf("MISMATCH %s\n", m.example.c_str());
    }
    throw ModelFailure();
}

// A value difference: with g_keepGoing (triage) recorded and the walk goes on.
bool g_keepGoing, g_modelFailed;
void ValueMismatch(const S_ctx &c, const std::string &what)
{
    if (!g_keepGoing)
        Mismatch(c, what);
    g_modelFailed = true;
    S_mismatch &m = g_mismatches[*c.key];
    const std::string text = g_model + ": " + Path() + " (" + g_currentClass + "): " + what;
    if (!m.count++)
        m.example = text;
    if (g_verbose && m.count <= 20)
        std::printf("MISMATCH %s\n", text.c_str());
}

std::string Hex(const uint8_t *b, uint32_t n)
{
    std::string s;
    char t[4];
    for (uint32_t i = 0; i < n && i < 24; ++i) {
        std::snprintf(t, sizeof t, "%02x", b[i]);
        s += t;
    }
    if (n > 24)
        s += "..";
    return s;
}

void CompareBytes(const S_ctx &c, const uint8_t *o, const uint8_t *p, uint32_t n, const S_type *t = nullptr)
{
    if (!std::memcmp(o, p, n))
        return;
    char text[160];
    if (t && t->tag == kTagBase && t->baseType == kBtFloat && n == 4) {
        float a, b;
        std::memcpy(&a, o, 4);
        std::memcpy(&b, p, 4);
        std::snprintf(text, sizeof text, "original %.9g (%08x), port %.9g (%08x)", a, *reinterpret_cast<const uint32_t *>(o),
                      b, *reinterpret_cast<const uint32_t *>(p));
        ValueMismatch(c, text);
        return;
    }
    uint32_t k = 0;
    while (o[k] == p[k])
        ++k;
    if (n <= 8) {
        uint64_t a = 0, b = 0;
        std::memcpy(&a, o, n);
        std::memcpy(&b, p, n);
        std::snprintf(text, sizeof text, "original %lld (0x%llx), port %lld (0x%llx)", static_cast<long long>(a),
                      static_cast<unsigned long long>(a), static_cast<long long>(b), static_cast<unsigned long long>(b));
        ValueMismatch(c, text);
        return;
    }
    const uint32_t from = k & ~7u;
    ValueMismatch(c, "bytes differ at " + std::to_string(k) + " of " + std::to_string(n) + ": original " +
                    Hex(o + from, std::min(16u, n - from)) + ", port " + Hex(p + from, std::min(16u, n - from)));
}

bool Readable(const void *address, size_t n)
{
    const auto *a = static_cast<const uint8_t *>(address);
    const uint8_t *end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                             PAGE_EXECUTE_WRITECOPY)))
            return false;
        a = static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

void Pair(const S_ctx &c, const void *o, const void *p)
{
    const auto a = reinterpret_cast<uintptr_t>(o), b = reinterpret_cast<uintptr_t>(p);
    auto x = g_o2p.find(a);
    if (x != g_o2p.end() && x->second.other != b)
        Mismatch(c, "the original's pointer goes where an earlier pointer went (" + ItemPath(x->second.item) +
                        "), the port's does not");
    auto y = g_p2o.find(b);
    if (y != g_p2o.end() && y->second.other != a)
        Mismatch(c, "the port's pointer goes where an earlier pointer went (" + ItemPath(y->second.item) +
                        "), the original's does not");
    if (x == g_o2p.end())
        g_o2p.emplace(a, S_pair{b, g_current});
    if (y == g_p2o.end())
        g_p2o.emplace(b, S_pair{a, g_current});
}

void CompareString(const S_ctx &c, const char *o, const char *p, size_t limit = 0x10000)
{
    if (!Readable(o, 1) || !Readable(p, 1))
        Mismatch(c, "unreadable string");
    const size_t a = strnlen(o, limit), b = strnlen(p, limit);
    if (a != b || std::memcmp(o, p, a))
        ValueMismatch(c, "original \"" + std::string(o, std::min<size_t>(a, 80)) + "\", port \"" +
                        std::string(p, std::min<size_t>(b, 80)) + "\"");
}

void CompareValue(ULONG type, uint8_t *o, uint8_t *p, const S_ctx &c);
void ComparePointer(ULONG pointee, uint8_t *o, uint8_t *p, const S_ctx &c);

void Enqueue(ULONG type, uint8_t *o, uint8_t *p)
{
    if (!g_walked.insert({reinterpret_cast<uintptr_t>(o), type}).second)
        return;
    std::string label;
    for (const std::string &s : g_inline) {
        if (!label.empty() && s[0] != '[')
            label += '.';
        label += s;
    }
    g_items.push_back({type, o, p, g_current, label});
    g_queue.push_back(static_cast<int>(g_items.size()) - 1);
}

// Rules: pointers whose target the types do not describe, by member.
using Rule = std::function<void(const S_ctx &, uint8_t *, uint8_t *)>;
std::map<std::string, Rule> g_rules;
// Hooks: run before an object's members, by class.
using Hook = std::function<void(uint8_t *, uint8_t *)>;
std::map<std::string, Hook> g_hooks;

bool Skipped(const S_field &f)
{
    if (!f.name.compare(0, 3, "pad"))
        return true;                        // padding / padNN: never written
    return f.name == "m_unused";            // the multiset's comparator byte
}

// Members the port never writes (and, as it reads the original, neither does the original): their
// differences are counted apart (g_soft), not failures - the examples show whether the original's
// values are heap garbage.
std::map<std::string, S_mismatch> g_soft;
// (`qualified`: the member inside the member holding it, "I3D_mesh::bboxMin.w".)
bool Soft(const S_field &f, const std::string &qualified)
{
    static const std::set<std::string> kUnset = {
        "I3D_mesh::unknown04", "I3D_mesh_level::unknown04", "I3D_mesh_level::unknown6C",
        // (The boxes' w words: nothing reads or writes them, whole-box copies carry what was there.)
        "I3D_bbox::minW", "I3D_bbox::maxW", "I3D_mesh::bboxMin.w", "I3D_mesh::bboxMax.w",
        "I3D_model::unknown138", "I3D_dummy::unknown144", "I3D_shadow::unknown144", "I3D_volume::unknown15C",
        "I3D_billboard::unknown218", "I3D_emitor::unknown288", "I3D_emitor::unknown2C4", "I3D_emitor::unknown2F4",
        "S_particle::unknown98", "I3D_joint::unknown16C", "I3D_light::unknown194", "I3D_lit_object::unknown364",
        "I3D_mirror::unknown248", "I3D_projector::unknown1EC", "I3D_projector::unknown24C", "I3D_scene::unknown168",
        "I3D_scene::unknown1B0", "I3D_scene::unknown208", "I3D_scene::unknown33C", "I3D_sector::unknown1C8",
        "S_skinData::unknown0C", "I3D_singlemesh::unknown24C", "I3D_target::unknown134", "I3D_user::unknown124",
        "I3D_visual::unknown124", "I3D_visual::unknown1DC", "C_morph::unknown1C",
        // (Written by the renderer only.)
        "I3D_visual::depth", "I3D_billboard::matrix", "I3D_portal::rectMin", "I3D_portal::rectMax",
        // (Set by the application: SetScreenProjection; the color's w never.)
        "I3D_scene::screenProjection", "I3D_scene::backgroundColorW",
        // (The renderer's: the weather's particle boxes, when drawn; a shadow's casters' box in light
        // space. Init leaves a shadow's color w and light direction w unset in the original.)
        "I3D_scene::weatherMatrices", "I3D_shadow::bounds", "I3D_shadow::color.w", "I3D_shadow::lightDir.w",
        // (The original's Load copies each effective color as 16 bytes of a stack temporary whose
        // fourth it never writes; a sound's and an emitter's velocities' w are unset locals.)
        "I3D_light::effectiveColorW", "I3D_light::effectiveSpecularW", "I3D_light::effectiveAmbientW",
        "I3D_sound::velocity.w", "I3D_emitor::startVelocity.w", "I3D_emitor::endVelocity.w",
        "I3D_portal::clipMin", "I3D_portal::clipMax",
        // (Never set: stack garbage in the original; the transformed vertices' w whatever the
        // transform left.)
        "I3D_portal::centerW", "I3D_portal::worldVertices.w", "I3D_occluder::centerW", "I3D_occluder::viewDepth",
        "I3D_occluder::vertices[].w", "I3D_occluder::worldVertices[].w",
        // (Init sets the colors' x, y, z; the original's w gets an unset local.)
        "I3D_lens::color.w", "I3D_lens::transmit.w", "I3D_mirror::color.w",
    };
    return kUnset.count(f.key) || kUnset.count(qualified);
}

// Caches compared only while their valid flag is set (the flags themselves are compared), by
// member: the predicate gets the original's object holding it.
std::map<std::string, std::function<bool(uint8_t *)>> g_valid;
std::map<std::string, long> g_notValid;
// Members of an embedded struct unset under a condition, by qualified member: the predicate gets
// the original's object holding the struct.
std::map<std::string, std::function<bool(uint8_t *)>> g_softIf;
// Arrays used up to a count (the rest unset), by member: the count from the original's object.
std::map<std::string, std::function<int(uint8_t *)>> g_used;

void CompareObject(const S_type &t, uint8_t *o, uint8_t *p, const S_ctx &outer);

void CompareVector(const S_type &t, uint8_t *o, uint8_t *p, const S_ctx &outer)
{
    const S_field *data = nullptr;
    for (const S_field &f : t.fields) {
        if (f.name == "m_data")
            data = &f;
        else
            CompareBytes(outer, o + f.offset, p + f.offset, GetType(f.type).size);
    }
    const int count = Field<int>(o, 8);
    uint8_t *od = Field<uint8_t *>(o, 0), *pd = Field<uint8_t *>(p, 0);
    if (!count)
        return;
    if (!od || !pd)
        Mismatch(outer, "C_vector without data");
    Pair(outer, od, pd);
    const ULONG elem = GetType(data->type).target;
    const uint32_t size = GetType(elem).size;
    if (!Readable(od, size * count) || !Readable(pd, size * count))
        Mismatch(outer, "unreadable C_vector data");
    for (int i = 0; i < count; ++i) {
        g_inline.push_back("[" + std::to_string(i) + "]");
        CompareValue(elem, od + i * size, pd + i * size, outer);
        g_inline.pop_back();
    }
}

void CompareTree(const S_ctx &c, uint8_t *o, uint8_t *p, ULONG valueType, int depth)
{
    // The nil node is the one without children.
    const bool oNil = !Field<void *>(o, 0), pNil = !Field<void *>(p, 0);
    if (oNil != pNil)
        Mismatch(c, oNil ? "original's subtree empty, port's not" : "port's subtree empty, original's not");
    if (oNil)
        return;
    if (depth > 64)
        Mismatch(c, "tree too deep");
    Pair(c, o, p);
    CompareBytes(c, o + 0x10, p + 0x10, 4);
    g_inline.push_back("value");
    CompareValue(valueType, o + 0x0C, p + 0x0C, c);
    g_inline.back() = "left";
    CompareTree(c, Field<uint8_t *>(o, 0), Field<uint8_t *>(p, 0), valueType, depth + 1);
    g_inline.back() = "right";
    CompareTree(c, Field<uint8_t *>(o, 8), Field<uint8_t *>(p, 8), valueType, depth + 1);
    g_inline.pop_back();
}

void CompareMultiset(const S_type &t, uint8_t *o, uint8_t *p, const S_ctx &outer)
{
    const S_field *head = nullptr;
    for (const S_field &f : t.fields) {
        if (f.name == "m_head")
            head = &f;
        else if (!Skipped(f))
            CompareBytes(outer, o + f.offset, p + f.offset, GetType(f.type).size);
    }
    uint8_t *oh = Field<uint8_t *>(o, head->offset), *ph = Field<uint8_t *>(p, head->offset);
    Pair(outer, oh, ph);
    const S_type &node = GetType(GetType(head->type).target);
    ULONG valueType = 0;
    for (const S_field &f : node.fields) {
        if (f.name == "value")
            valueType = f.type;
    }
    // The head: its parent is the root, its left / right the first / last node.
    g_inline.push_back("root");
    CompareTree(outer, Field<uint8_t *>(oh, 4), Field<uint8_t *>(ph, 4), valueType, 0);
    g_inline.back() = "first";
    Pair(outer, Field<uint8_t *>(oh, 0), Field<uint8_t *>(ph, 0));
    g_inline.back() = "last";
    Pair(outer, Field<uint8_t *>(oh, 8), Field<uint8_t *>(ph, 8));
    g_inline.pop_back();
}

void CompareObject(const S_type &t, uint8_t *o, uint8_t *p, const S_ctx &outer)
{
    Pair(outer, o, p);
    if (!t.name.compare(0, 15, "ls3d::C_vector<")) {
        CompareVector(t, o, p, outer);
        return;
    }
    if (!t.name.compare(0, 15, "ls3d::Multiset<")) {
        CompareMultiset(t, o, p, outer);
        return;
    }
    auto hook = g_hooks.find(t.name);
    if (hook != g_hooks.end())
        hook->second(o, p);
    for (const S_field &f : t.fields) {
        if (Skipped(f))
            continue;
        if (f.overlapped) {
            ++g_overlapped[f.key];
            continue;
        }
        // (The class declaring the member: a base's subobject.)
        uint8_t *so = o + f.offset - f.local, *sp = p + f.offset - f.local;
        auto valid = g_valid.find(f.key);
        if (valid != g_valid.end() && !valid->second(so)) {
            ++g_notValid[f.key];
            continue;
        }
        const std::string qualified = *outer.key + "." + f.name;
        auto softIf = g_softIf.find(qualified);
        if (Soft(f, qualified) || (softIf != g_softIf.end() && softIf->second(outer.oObj))) {
            const uint32_t n = GetType(f.type).size;
            if (std::memcmp(o + f.offset, p + f.offset, n)) {
                S_mismatch &m = g_soft[outer.key->find("::(object)") == std::string::npos ? qualified : f.key];
                if (!m.count++)
                    m.example = g_model + ": " + Path() + "." + f.name + ": original " + Hex(o + f.offset, n) +
                                ", port " + Hex(p + f.offset, n);
                static const uint8_t kZero[64] = {};
                m.originalZero += n <= sizeof kZero && !std::memcmp(o + f.offset, kZero, n);
            }
            continue;
        }
        g_inline.push_back(f.name);
        const S_ctx c = {&f.key, so, sp};
        auto used = g_used.find(f.key);
        if (used != g_used.end()) {
            const S_type &array = GetType(f.type);
            const uint32_t size = GetType(array.target).size;
            const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(std::max(used->second(so), 0)), array.count);
            for (uint32_t i = 0; i < n; ++i) {
                g_inline.push_back("[" + std::to_string(i) + "]");
                CompareValue(array.target, o + f.offset + i * size, p + f.offset + i * size, c);
                g_inline.pop_back();
            }
        } else {
            CompareValue(f.type, o + f.offset, p + f.offset, c);
        }
        g_inline.pop_back();
    }
}

// `n` elements of `type` at o / p (the targets of pointers to arrays).
void CompareElements(const S_ctx &c, ULONG type, uint8_t *o, uint8_t *p, int n)
{
    const S_type &t = GetType(type);
    const uint32_t size = t.size;
    if (n <= 0)
        return;
    if (!Readable(o, size * n) || !Readable(p, size * n))
        Mismatch(c, "unreadable array");
    if (t.tag == kTagBase || t.tag == kTagEnum) {
        CompareBytes(c, o, p, size * n);
        return;
    }
    const std::string key = *c.key + "[]";      // (not the member's rule again)
    const S_ctx e = {&key, c.oObj, c.pObj};
    for (int i = 0; i < n; ++i) {
        g_inline.push_back("[" + std::to_string(i) + "]");
        CompareValue(type, o + i * size, p + i * size, e);
        g_inline.pop_back();
    }
}

void CompareValue(ULONG type, uint8_t *o, uint8_t *p, const S_ctx &c)
{
    const S_type &t = GetType(type);
    switch (t.tag) {
    case kTagBase:
    case kTagEnum:
        CompareBytes(c, o, p, t.size, &t);
        break;
    case kTagPointer:
        ComparePointer(t.target, Field<uint8_t *>(o, 0), Field<uint8_t *>(p, 0), c);
        break;
    case kTagArray: {
        const S_type &e = GetType(t.target);
        if (e.tag == kTagBase && e.baseType == kBtChar && e.size == 1) {
            CompareString(c, reinterpret_cast<char *>(o), reinterpret_cast<char *>(p), t.count);
            break;
        }
        if ((e.tag == kTagBase || e.tag == kTagEnum) && !std::memcmp(o, p, t.size))
            break;
        for (uint32_t i = 0; i < t.count; ++i) {
            g_inline.push_back("[" + std::to_string(i) + "]");
            CompareValue(t.target, o + i * e.size, p + i * e.size, c);
            g_inline.pop_back();
        }
        break;
    }
    case kTagUDT:
        CompareObject(t, o, p, c);
        break;
    default:
        break;
    }
}

void CompareFakeTexture(const S_ctx &c, FakeTexture *o, FakeTexture *p)
{
    if (o->format != p->format || o->levels.size() != p->levels.size())
        Mismatch(c, "Direct3D textures: format " + std::to_string(o->format) + " / " + std::to_string(p->format) +
                        ", levels " + std::to_string(o->levels.size()) + " / " + std::to_string(p->levels.size()));
    for (size_t i = 0; i < o->levels.size(); ++i) {
        g_inline.push_back("level" + std::to_string(i));
        const FakeSurface *a = o->levels[i], *b = p->levels[i];
        CompareBytes(c, reinterpret_cast<const uint8_t *>(&a->desc), reinterpret_cast<const uint8_t *>(&b->desc),
                     sizeof a->desc);
        CompareBytes(c, a->bits.data(), b->bits.data(), static_cast<uint32_t>(a->bits.size()));
        g_inline.pop_back();
    }
}

// Pointers to Direct3D objects (the fakes) and to the engine's singletons. True when handled.
bool CompareSpecial(const S_type &t, uint8_t *o, uint8_t *p, const S_ctx &c)
{
    const std::string &n = t.name;
    if (n == "IDirect3DVertexBuffer8") {
        auto *a = static_cast<FakeVB *>(reinterpret_cast<IDirect3DVertexBuffer8 *>(o));
        auto *b = static_cast<FakeVB *>(reinterpret_cast<IDirect3DVertexBuffer8 *>(p));
        CompareBytes(c, reinterpret_cast<uint8_t *>(&a->desc), reinterpret_cast<uint8_t *>(&b->desc), sizeof a->desc);
        if (a->data.size() != b->data.size())
            Mismatch(c, "vertex buffer sizes");
        g_inline.push_back("(bytes)");
        CompareBytes(c, a->data.data(), b->data.data(), static_cast<uint32_t>(a->data.size()));
        g_inline.pop_back();
        return true;
    }
    if (n == "IDirect3DIndexBuffer8") {
        auto *a = static_cast<FakeIB *>(reinterpret_cast<IDirect3DIndexBuffer8 *>(o));
        auto *b = static_cast<FakeIB *>(reinterpret_cast<IDirect3DIndexBuffer8 *>(p));
        CompareBytes(c, reinterpret_cast<uint8_t *>(&a->desc), reinterpret_cast<uint8_t *>(&b->desc), sizeof a->desc);
        if (a->data.size() != b->data.size())
            Mismatch(c, "index buffer sizes");
        g_inline.push_back("(bytes)");
        CompareBytes(c, a->data.data(), b->data.data(), static_cast<uint32_t>(a->data.size()));
        g_inline.pop_back();
        return true;
    }
    if (n == "IDirect3DTexture8" || n == "IDirect3DBaseTexture8") {
        CompareFakeTexture(c, static_cast<FakeTexture *>(reinterpret_cast<IDirect3DTexture8 *>(o)),
                           static_cast<FakeTexture *>(reinterpret_cast<IDirect3DTexture8 *>(p)));
        return true;
    }
    if (n == "IDirect3DDevice8") {
        if (o != reinterpret_cast<uint8_t *>(static_cast<IDirect3DDevice8 *>(&g_side[0].device)) ||
            p != reinterpret_cast<uint8_t *>(static_cast<IDirect3DDevice8 *>(&g_side[1].device)))
            Mismatch(c, "not the side's device");
        return true;
    }
    if (n == "I3D_driver") {
        if (o != reinterpret_cast<uint8_t *>(g_side[0].driver) || p != reinterpret_cast<uint8_t *>(g_side[1].driver))
            Mismatch(c, "not the side's driver");
        return true;
    }
    return !n.compare(0, 7, "IDirect") || n == "IGraph" || n == "ISND_driver";
}

void ComparePointer(ULONG pointee, uint8_t *o, uint8_t *p, const S_ctx &c)
{
    if (!o || !p) {
        if (o != p)
            Mismatch(c, o ? "original's pointer set, port's null" : "original's pointer null, port's set");
        return;
    }
    Pair(c, o, p);
    auto rule = g_rules.find(*c.key);
    if (rule != g_rules.end()) {
        rule->second(c, o, p);
        return;
    }
    const S_type &t = GetType(pointee);
    switch (t.tag) {
    case kTagBase:
        if (t.baseType == kBtChar && t.size == 1)
            CompareString(c, reinterpret_cast<char *>(o), reinterpret_cast<char *>(p));
        else
            ++g_unhandled[*c.key];
        return;
    case kTagUDT:
        if (!CompareSpecial(t, o, p, c)) {
            if (!t.polymorphic)
                ++g_plainTargets[*c.key + " -> " + t.name];
            Enqueue(t.id, o, p);
        }
        return;
    case kTagFunctionType:
        return;
    default:
        ++g_unhandled[*c.key];
        return;
    }
}

// The class of a polymorphic object (the port's vtable's), its vtables paired.
ULONG DynamicType(ULONG type, uint8_t *o, uint8_t *p, const S_ctx &c)
{
    const S_type &t = GetType(type);
    if (t.tag != kTagUDT || !t.polymorphic)
        return type;
    const uintptr_t vo = Field<uintptr_t>(o, 0), vp = Field<uintptr_t>(p, 0);
    auto x = g_vtO2P.find(vo);
    auto y = g_vtP2O.find(vp);
    if ((x != g_vtO2P.end() && x->second != vp) || (y != g_vtP2O.end() && y->second != vo)) {
        char text[200];
        std::snprintf(text, sizeof text, "classes differ: original vtable 0x%08x, port %s", ToOrigVa(vo),
                      PortSymbolAt(vp).c_str());
        Mismatch(c, text);
    }
    g_vtO2P[vo] = vp;
    g_vtP2O[vp] = vo;
    auto known = g_vtType.find(vp);
    if (known != g_vtType.end())
        return known->second;
    ULONG dyn = type;
    std::string name = PortSymbolAt(vp);
    const size_t at = name.find("::`vftable'");
    if (at != std::string::npos && name.find("{for") == std::string::npos) {
        name = name.substr(0, at);
        if (!name.compare(0, 6, "const "))
            name = name.substr(6);
        if (const ULONG id = TypeByName(name))
            dyn = id;
    }
    g_vtType[vp] = dyn;
    return dyn;
}

void WalkFrom(ULONG type, uint8_t *o, uint8_t *p, const char *label)
{
    g_items.clear();
    g_queue.clear();
    g_walked.clear();
    g_o2p.clear();
    g_p2o.clear();
    g_uvAllocs.clear();
    g_inline.clear();
    g_current = -1;
    g_items.push_back({type, o, p, -1, label});
    g_walked.insert({reinterpret_cast<uintptr_t>(o), type});
    g_queue.push_back(0);
    static const std::string kObject = "(object)";
    while (!g_queue.empty()) {
        g_current = g_queue.front();
        g_queue.pop_front();
        S_item item = g_items[g_current];
        const S_ctx c = {&kObject, item.o, item.p};
        const ULONG dyn = DynamicType(item.type, item.o, item.p, c);
        if (dyn != item.type && !g_walked.insert({reinterpret_cast<uintptr_t>(item.o), dyn}).second)
            continue;                       // (reached before through a pointer of another class)
        const S_type &t = GetType(dyn);
        if (!Readable(item.o, t.size) || !Readable(item.p, t.size))
            Mismatch(c, "unreadable object");
        ++g_classes[t.name];
        g_currentClass = t.name;
        const std::string key = t.name + "::(object)";
        CompareObject(t, item.o, item.p, {&key, item.o, item.p});
    }
}

// ---------------------------------------------------------------- rules

void CompareAnimationSet(const S_ctx &c, uint8_t *o, uint8_t *p)
{
    // (Slots: 0x08 GetLength, 0x0C NumTracks, 0x10 GetTrackName, 0x18 - 0x38 the keys.)
    auto same = [&](const char *what, long a, long b) {
        if (a != b)
            Mismatch(c, std::string(what) + ": original " + std::to_string(a) + ", port " + std::to_string(b));
    };
    same("length", CallV<int>(o, 0x08), CallV<int>(p, 0x08));
    const int tracks = CallV<int>(o, 0x0C);
    same("tracks", tracks, CallV<int>(p, 0x0C));
    for (int k = 0; k < tracks; ++k) {
        g_inline.push_back("track" + std::to_string(k));
        CompareString(c, CallV<const char *>(o, 0x10, k), CallV<const char *>(p, 0x10, k));
        for (int kind = 0; kind < 3; ++kind) {
            const int n = CallV<int>(o, 0x18 + kind * 8, k);
            same("keys", n, CallV<int>(p, 0x18 + kind * 8, k));
            for (int key = 0; key < n; ++key) {
                int ta = 0, tb = 0;
                float va[4] = {}, vb[4] = {};
                const int ra = CallV<int>(o, 0x1C + kind * 8, k, key, &ta, static_cast<void *>(va));
                const int rb = CallV<int>(p, 0x1C + kind * 8, k, key, &tb, static_cast<void *>(vb));
                same("key result", ra, rb);
                same("key time", ta, tb);
                CompareBytes(c, reinterpret_cast<uint8_t *>(va), reinterpret_cast<uint8_t *>(vb), sizeof va);
            }
        }
        int is16a = 0, is16b = 0;
        const int notes = CallV<int>(o, 0x30, k, &is16a);
        same("note keys", notes, CallV<int>(p, 0x30, k, &is16b));
        same("note size", is16a, is16b);
        for (int key = 0; key < notes; ++key) {
            int ta = 0, tb = 0;
            uint32_t va = 0, vb = 0;
            const int slot = is16a ? 0x34 : 0x38;
            same("note result", CallV<int>(o, slot, k, key, &ta, static_cast<void *>(&va)),
                 CallV<int>(p, slot, k, key, &tb, static_cast<void *>(&vb)));
            same("note time", ta, tb);
            if (is16a) {
                same("note value", static_cast<long>(va), static_cast<long>(vb));
            } else {
                // A 32-bit note is a pointer into the blob (Open turns the file's offsets into them).
                const auto *da = reinterpret_cast<const I3D_animation_set *>(o)->data;
                const auto *db = reinterpret_cast<const I3D_animation_set *>(p)->data;
                same("note offset", static_cast<long>(va - reinterpret_cast<uintptr_t>(da)),
                     static_cast<long>(vb - reinterpret_cast<uintptr_t>(db)));
                CompareString(c, reinterpret_cast<const char *>(static_cast<uintptr_t>(va)),
                              reinterpret_cast<const char *>(static_cast<uintptr_t>(vb)));
            }
        }
        g_inline.pop_back();
    }
}

// The target type of a pointer member ("Class::member").
ULONG PointeeOf(const std::string &key)
{
    const size_t at = key.rfind("::");
    const S_type &t = GetType(TypeByName(key.substr(0, at)));
    for (const S_field &f : t.fields) {
        if (f.key == key)
            return GetType(f.type).target;
    }
    std::printf("no member %s\n", key.c_str());
    std::exit(2);
}

// A pointer member to an array: `count` elements (from the original's object holding it).
void ArrayRule(const std::string &key, std::function<int(uint8_t *)> count)
{
    g_rules[key] = [key, count](const S_ctx &c, uint8_t *o, uint8_t *p) {
        static std::map<std::string, ULONG> s_types;
        auto it = s_types.find(key);
        if (it == s_types.end())
            it = s_types.emplace(key, PointeeOf(key)).first;
        CompareElements(c, it->second, o, p, count(c.oObj));
    };
}

void SetUpRules()
{
    // Occluders: the geometry (the local vertices' w unset), the edges (three per face); the
    // occlusion volume is the renderer's (its planes up to their counts, the near plane while
    // active).
    auto occ = [](uint8_t *o) { return reinterpret_cast<I3D_occluder *>(o); };
    ArrayRule("I3D_occluder::vertices", [occ](uint8_t *o) { return occ(o)->numVertices; });
    ArrayRule("I3D_occluder::worldVertices", [occ](uint8_t *o) { return occ(o)->numVertices; });
    ArrayRule("I3D_occluder::faces", [occ](uint8_t *o) { return occ(o)->numFaces; });
    ArrayRule("I3D_occluder::facePlanes", [occ](uint8_t *o) { return occ(o)->numFaces; });
    ArrayRule("I3D_occluder::faceFlags", [occ](uint8_t *o) { return occ(o)->numFaces; });
    ArrayRule("I3D_occluder::neighbors", [occ](uint8_t *o) { return occ(o)->numFaces * 3; });
    ArrayRule("I3D_occluder::neighborEdges", [occ](uint8_t *o) { return occ(o)->numFaces * 3; });
    g_used["I3D_occluder::silhouettePlanes"] = [occ](uint8_t *o) { return occ(o)->numSilhouettePlanes; };
    g_used["I3D_occluder::frontPlanes"] = [occ](uint8_t *o) { return occ(o)->numFrontPlanes; };
    g_valid["I3D_occluder::nearPlane"] = [occ](uint8_t *o) { return (occ(o)->occluderFlags & OCCF_ACTIVE) != 0; };
    // Sectors: the hull's vertices, faces and planes.
    auto sct = [](uint8_t *o) { return reinterpret_cast<I3D_sector *>(o); };
    ArrayRule("I3D_sector::vertices", [sct](uint8_t *o) { return sct(o)->numVertices; });
    ArrayRule("I3D_sector::faces", [sct](uint8_t *o) { return sct(o)->numFaces; });
    ArrayRule("I3D_sector::planes", [sct](uint8_t *o) { return sct(o)->numPlanes; });
    // Mirrors: the vertices; the plane's sphere, inverse and reflection while valid.
    auto mir = [](uint8_t *o) { return reinterpret_cast<I3D_mirror *>(o); };
    ArrayRule("I3D_mirror::vertices", [mir](uint8_t *o) { return mir(o)->numVertices; });
    for (const char *m : {"I3D_mirror::planeRadius2", "I3D_mirror::planeInverse", "I3D_mirror::reflection"})
        g_valid[m] = [mir](uint8_t *o) { return (mir(o)->mirrorFlags & MIRF_REFLECTION_VALID) != 0; };
    // Morphs: the weights (channels x targets; the normalized ones after them, made when drawn),
    // the definition's channels (levels x channels), each with its vertices' indices and
    // targets (the targets x vertices x 0x18 bytes: the morph's count, the hook).
    auto mor = [](uint8_t *o) { return reinterpret_cast<C_morph *>(o); };
    ArrayRule("C_morph::weights", [mor](uint8_t *o) { return mor(o)->numChannels * mor(o)->numTargets; });
    g_rules["C_morph::normalized"] = [mor](const S_ctx &c, uint8_t *o, uint8_t *p) {
        if (o - reinterpret_cast<uint8_t *>(mor(c.oObj)->weights) != p - reinterpret_cast<uint8_t *>(mor(c.pObj)->weights))
            Mismatch(c, "not at the same place after the weights");
    };
    ArrayRule("S_morphDef::channels", [](uint8_t *o) {
        return reinterpret_cast<S_morphDef *>(o)->numLevels * reinterpret_cast<S_morphDef *>(o)->numChannels;
    });
    g_rules["S_morphChannel::indices"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        CompareBytes(c, o, p, static_cast<uint32_t>(reinterpret_cast<S_morphChannel *>(c.oObj)->numVertices * 2));
    };
    g_rules["S_morphChannel::targets"] = [](const S_ctx &, uint8_t *, uint8_t *) {};
    auto morphTargets = [](uint8_t *o, uint8_t *p, C_morph *mo, C_morph *mp) {
        if (!mo->def || !mp->def || !mo->def->channels || !mp->def->channels)
            return;
        static const std::string kKey = "S_morphChannel::targets";
        const S_ctx c = {&kKey, o, p};
        for (int i = 0; i < mo->def->numLevels * mo->def->numChannels; ++i) {
            const S_morphChannel &a = mo->def->channels[i], &b = mp->def->channels[i];
            if (!a.targets || !b.targets || a.numVertices != b.numVertices)
                continue;
            g_inline.push_back("def.channels[" + std::to_string(i) + "].targets");
            CompareBytes(c, reinterpret_cast<uint8_t *>(a.targets), reinterpret_cast<uint8_t *>(b.targets),
                         static_cast<uint32_t>(mo->numTargets * a.numVertices * 0x18));
            g_inline.pop_back();
        }
    };
    g_hooks["I3D_morph"] = [morphTargets](uint8_t *o, uint8_t *p) {
        morphTargets(o, p, static_cast<C_morph *>(reinterpret_cast<I3D_morph *>(o)),
                     static_cast<C_morph *>(reinterpret_cast<I3D_morph *>(p)));
    };
    g_hooks["I3D_singlemorph"] = [morphTargets](uint8_t *o, uint8_t *p) {
        morphTargets(o, p, static_cast<C_morph *>(reinterpret_cast<I3D_singlemorph *>(o)),
                     static_cast<C_morph *>(reinterpret_cast<I3D_singlemorph *>(p)));
    };

    // scaleW is read from the file: frames made, not loaded (the model or scene itself, the
    // scene's own sectors), have it unset in the original. Loaded frames have an owner.
    // Occluders a sector makes from its own geometry get an unset fourth dword with their scale;
    // an area's Load reads nothing (not even the frame's data).
    g_valid["I3D_frame::scaleW"] = [](uint8_t *o) {
        const auto *f = reinterpret_cast<I3D_frame *>(o);
        if (f->type == FRAME_OCCLUDER && reinterpret_cast<const I3D_occluder *>(f)->ownerSector)
            return false;
        if (f->type == FRAME_AREA)
            return false;
        return f->ownerModel != nullptr;
    };
    g_valid["I3D_joint::modelMatrix"] = [](uint8_t *o) {
        return (reinterpret_cast<I3D_joint *>(o)->jointFlags & JOINTF_MODEL_MATRIX_VALID) != 0;
    };
    // A level's box w: the first vertex's normal x (ComputeBounds); unset in a texture level.
    for (const char *m : {"I3D_mesh_level::bboxMin.w", "I3D_mesh_level::bboxMax.w"})
        g_softIf[m] = [](uint8_t *o) { return reinterpret_cast<I3D_mesh_level *>(o)->numVertices == 0; };
    // (The original's constructor leaves the stream record unset: only Create of a streamed
    // texture writes it.)
    g_valid["ITexture::stream"] = [](uint8_t *o) { return (reinterpret_cast<ITexture *>(o)->flags & ITC_STREAMED) != 0; };
    g_hooks["ITexture"] = [](uint8_t *o, uint8_t *) { ++g_formats[reinterpret_cast<ITexture *>(o)->format]; };
    g_hooks["I3D_target"] = [](uint8_t *o, uint8_t *) { g_targetFrames += reinterpret_cast<I3D_target *>(o)->frames.Size(); };
    // ITC_KEEP_PIXELS: level 0 as 32-bit BGRA, or a 1-bit mask.
    g_rules["ITexture::pixels"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        const auto *t = reinterpret_cast<ITexture *>(c.oObj);
        const uint32_t n = (t->flags & ITC_PIXEL_MASK) ? (t->width * t->height + 7) / 8 : t->width * t->height * 4u;
        CompareBytes(c, o, p, n);
    };

    // Skinned meshes: the skin's parents up to its joint count (the rest unset on both sides),
    // its joints, and per level the vertices' joints (the hook: the level gives the count).
    g_used["S_skinData::parents"] = [](uint8_t *o) { return reinterpret_cast<S_skinData *>(o)->numJoints; };
    // A portal's vertices (up to 8).
    g_used["I3D_portal::vertices"] = [](uint8_t *o) { return reinterpret_cast<I3D_portal *>(o)->numVertices; };
    g_used["I3D_portal::worldVertices"] = [](uint8_t *o) { return reinterpret_cast<I3D_portal *>(o)->numVertices; };
    g_rules["S_skinData::joints"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        static const ULONG kJoint = TypeByName("S_skinJoint");
        CompareElements(c, kJoint, o, p, reinterpret_cast<S_skinData *>(c.oObj)->numJoints);
    };
    g_rules["S_skinData::vertexJoints"] = [](const S_ctx &, uint8_t *, uint8_t *) {};
    g_hooks["I3D_singlemesh"] = [](uint8_t *o, uint8_t *p) {
        auto *so = reinterpret_cast<I3D_singlemesh *>(o), *sp = reinterpret_cast<I3D_singlemesh *>(p);
        if (!so->skin || !sp->skin || !so->mesh)
            return;
        static const std::string kKey = "S_skinData::vertexJoints";
        const S_ctx c = {&kKey, o, p};
        for (uint32_t i = 0; i < so->mesh->numLevels && i < I3D_mesh::kMaxLevels; ++i) {
            uint8_t *a = so->skin->vertexJoints[i], *b = sp->skin->vertexJoints[i];
            if (!a || !b || !so->mesh->levels[i])
                continue;
            g_inline.push_back("skin.vertexJoints[" + std::to_string(i) + "]");
            CompareBytes(c, a, b, static_cast<uint32_t>(std::max(so->mesh->levels[i]->numVertices, so->skin->numWeighted[i]) * 2));
            g_inline.pop_back();
        }
    };
    g_rules["I3D_singlemesh::joints"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        static const ULONG kJointPointer = [] {
            const S_type &sm = GetType(TypeByName("I3D_singlemesh"));
            for (const S_field &f : sm.fields) {
                if (f.name == "joints")
                    return GetType(f.type).target;          // I3D_joint *
            }
            return 0ul;
        }();
        CompareElements(c, kJointPointer, o, p, reinterpret_cast<I3D_singlemesh *>(c.oObj)->numJoints);
    };
    // A level's extra per-vertex data: ExtraStride (slot 0x40) bytes per vertex.
    g_rules["I3D_mesh_level::extraData"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        const int stride = CallV<int>(c.oObj, 0x40);
        if (stride != CallV<int>(c.pObj, 0x40))
            Mismatch(c, "extra data strides");
        const int n = Field<int>(c.oObj, 0x54);
        CompareBytes(c, o, p, static_cast<uint32_t>(stride * n));
    };
    // Texture levels keep level 0's UVs where the vertex buffer goes (see the I3D_mesh hook).
    g_rules["ls3d::S_VBAlloc::buffer"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        if (g_uvAllocs.count(reinterpret_cast<uintptr_t>(c.oObj)))
            return;
        static const ULONG kBuffer = TypeByName("ls3d::C_VertexBuffer");
        Enqueue(kBuffer, o, p);
    };
    g_hooks["I3D_mesh"] = [](uint8_t *o, uint8_t *p) {
        auto *mo = reinterpret_cast<I3D_mesh *>(o);
        for (uint32_t i = 1; i < mo->numLevels && i < I3D_mesh::kMaxLevels; ++i) {
            I3D_mesh_level *lo = mo->levels[i], *lp = reinterpret_cast<I3D_mesh *>(p)->levels[i];
            if (!lo || !lp || lo->vertices.vb || !lo->vertices.buffer || !mo->levels[0])
                continue;
            static const std::string kKey = "I3D_mesh::levels (texture level UVs)";
            const S_ctx c = {&kKey, o, p};
            if (lp->vertices.vb || !lp->vertices.buffer)
                Mismatch(c, "texture level on one side only");
            g_inline.push_back("levels[" + std::to_string(i) + "].uv");
            CompareBytes(c, reinterpret_cast<uint8_t *>(lo->vertices.buffer), reinterpret_cast<uint8_t *>(lp->vertices.buffer),
                         static_cast<uint32_t>(mo->levels[0]->numVertices * 8));
            g_inline.pop_back();
            g_uvAllocs.insert(reinterpret_cast<uintptr_t>(&lo->vertices));
        }
    };
    // Locked buffers: the offset into the fake's bytes.
    g_rules["ls3d::C_VertexBuffer::lockedData"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        auto *a = static_cast<FakeVB *>(reinterpret_cast<ls3d::C_VertexBuffer *>(c.oObj)->vb);
        auto *b = static_cast<FakeVB *>(reinterpret_cast<ls3d::C_VertexBuffer *>(c.pObj)->vb);
        if (!a || !b || o - a->data.data() != p - b->data.data())
            Mismatch(c, "locked at different offsets");
    };
    g_rules["ls3d::C_IndexBuffer::lockedData"] = [](const S_ctx &c, uint8_t *o, uint8_t *p) {
        auto *a = static_cast<FakeIB *>(reinterpret_cast<ls3d::C_IndexBuffer *>(c.oObj)->ib);
        auto *b = static_cast<FakeIB *>(reinterpret_cast<ls3d::C_IndexBuffer *>(c.pObj)->ib);
        if (!a || !b || o - a->data.data() != p - b->data.data())
            Mismatch(c, "locked at different offsets");
    };
    // A block's owner (an allocation record inside a level or face group): paired only.
    g_rules["ls3d::S_bufferBlock::owner"] = [](const S_ctx &, uint8_t *, uint8_t *) {};
    g_rules["I3D_animation_set::data"] = [](const S_ctx &c, uint8_t *, uint8_t *) {
        CompareAnimationSet(c, c.oObj, c.pObj);
    };
    // A stage's keys: pointers into the set's blob, at the key times (counts beside them; the
    // keys themselves are compared through the set).
    static const char *const kKeys[4] = {"positions", "rotations", "scales", "notes"};
    for (int k = 0; k < 4; ++k) {
        g_rules[std::string("I3D_anim_keys::") + kKeys[k]] = [k](const S_ctx &c, uint8_t *o, uint8_t *p) {
            const int n = Field<uint16_t>(c.oObj, 0x10 + 2 * k);
            CompareBytes(c, o, p, static_cast<uint32_t>(2 * n));
        };
    }
}

// ---------------------------------------------------------------- set-up

std::string ExeDir()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s = path;
    return s.substr(0, s.find_last_of('\\'));
}

void MountArchives(const std::string &dir)
{
    // The game's archives in its priority order, with its keys (src/game/app.cpp).
    static const struct {
        const char *name;
        uint32_t low, high;
    } kArchives[] = {
        {"PatchX03.dta", 0xA0A0A0A4, 0xA0A0A0A0}, {"PatchX02.dta", 0xA0A0A0A3, 0xA0A0A0A0},
        {"PatchX01.dta", 0xA0A0A0A2, 0xA0A0A0A0}, {"SabreSquadron.dta", 0xA0A0A0A1, 0xA0A0A0A0},
        {"LangEnglish.dta", 0xA0A0A0A0, 0xA0A0A0A0}, {"Patch.dta", 0x5D805270, 0x10ACB252},
        {"Models.dta", 0x5D805259, 0x10ACB252}, {"Missions.dta", 0x7654A3F0, 0x22BCDA98},
        {"Maps.dta", 0xB438D0A5, 0xF26527FA}, {"Sounds.dta", 0x4FE85106, 0x8D2965CA},
        {"Scripts.dta", 0x0AB4E72D, 0xCF761298}, {"Others.dta", 0xEA85902A, 0x65F7AB23},
        {"Maps_C.dta", 0xB038D0A0, 0xE26520FA}, {"Maps_U.dta", 0xB038D1A1, 0xF26520FA},
    };
    for (const auto &a : kArchives) {
        const std::string path = dir + "\\" + a.name;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        C_DtaArchive *archive = dtaCreate(path.c_str());
        if (!archive) {
            std::printf("cannot mount %s\n", path.c_str());
            continue;
        }
        archive->XorKey(a.low, a.high);
        const uint64_t key = archive->GetKey();
        archive->XorKey(static_cast<uint32_t>(key) ^ a.low, static_cast<uint32_t>(key >> 32) ^ a.high);
    }
}

void Configure(int side)
{
    Global<bool>(side, kLogOff) = true;
    Global<IDirect3DDevice8 *>(side, kDevice) = &g_side[side].device;
    Global<D3DFORMAT>(side, kFmt16) = D3DFMT_R5G6B5;
    Global<D3DFORMAT>(side, kFmt1) = D3DFMT_A1R5G5B5;
    Global<D3DFORMAT>(side, kFmtAlpha) = D3DFMT_A4R4G4B4;
    Global<D3DFORMAT>(side, kFmtTrue) = D3DFMT_A8R8G8B8;
    Global<D3DFORMAT>(side, kFmtDxt1) = D3DFMT_DXT1;
    Global<D3DFORMAT>(side, kFmtDxt3) = D3DFMT_DXT3;
    Global<D3DFORMAT>(side, kFmtDxt5) = D3DFMT_DXT5;
    Global<D3DFORMAT>(side, kFmtBump) = D3DFMT_V8U8;
    Global<uint32_t>(side, kCaps) = IGCAPS_DXT;
    Global<int>(side, kMaxW) = 2048;
    Global<int>(side, kMaxH) = 2048;
}

void FindFiles(const std::string &dir, const std::string &prefix, const char *filter, std::vector<std::string> &out,
               const char *pattern = "\\*.4ds")
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::string name = prefix + fd.cFileName;
        if (!filter || std::strstr(name.c_str(), filter))
            out.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// The models and shadow files, then the missions' files (scenes, maps, loading screens).
std::vector<std::string> ModelNames(const std::string &unpacked, const char *filter)
{
    std::vector<std::string> out;
    FindFiles(unpacked + "\\MODELS", "Models\\", filter, out);
    FindFiles(unpacked + "\\MODELS", "Models\\", filter, out, "\\*.6ds");
    std::sort(out.begin(), out.end());
    std::vector<std::string> missions;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((unpacked + "\\MISSIONS\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
                FindFiles(unpacked + "\\MISSIONS\\" + fd.cFileName,
                          std::string("Missions\\") + fd.cFileName + "\\", filter, missions);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(missions.begin(), missions.end());
    out.insert(out.end(), missions.begin(), missions.end());
    return out;
}

// ---------------------------------------------------------------- synthetic files

// Model files for what the game's never have, written to the work folder (dtaOpen looks on the
// disk first): the frame types and visuals the game's files lack (lights of every type,
// cameras, sounds, user frames, volumes, shadows, a model inside a model, lit objects,
// projectors, emitters, an area), meshes with several levels, extra per-vertex data, a texture
// level and a shared mesh, targets listing frames (the loader turns the indices into frames and
// flags them; 0 is dropped, one past the last frame makes the file corrupt), and corrupt files
// (a parent index ahead of its frame, unknown frame and visual types, material IDs past the
// materials, too many mesh levels).
struct S_bytes {
    std::vector<uint8_t> v;
    void Put(const void *p, size_t n) { v.insert(v.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); }
    void U8(int x) { v.push_back(static_cast<uint8_t>(x)); }
    void U16(int x)
    {
        const uint16_t u = static_cast<uint16_t>(x);
        Put(&u, 2);
    }
    void U32(uint32_t x) { Put(&x, 4); }
    void Floats(std::initializer_list<float> f)
    {
        for (float x : f)
            Put(&x, 4);
    }
    void Name(const char *s)
    {
        U8(static_cast<int>(std::strlen(s)));
        Put(s, std::strlen(s));
    }
};
using Writer = std::function<void(S_bytes &)>;

// A material block: flags, the colors (diffuse, ambient, specular, emissive, with w), power and
// opacity, the texture's name, for alpha (without the 0xa1000000 flags) the opacity map's.
Writer Material(uint32_t flags, const char *texture, const char *alpha = "")
{
    return [=](S_bytes &b) {
        b.U32(flags);
        b.Floats({0.8f, 0.7f, 0.6f, 0, 0.2f, 0.2f, 0.25f, 0, 1, 1, 1, 0, 0, 0.1f, 0, 0, 12.0f, 1.0f});
        b.Name(texture);
        if (*texture && (flags & MATF_ALPHA) && !(flags & 0xa1000000))
            b.Name(alpha);
    };
}

// A mesh (LoadMesh): an earlier frame's (`shared`, 1-based), or levels of vertices (0xffff: a
// texture level, level 0's UVs), each with face groups (faces, material ID).
struct S_level {
    float lod;
    uint32_t extraFormat;
    int vertices;
    std::vector<std::pair<int, int>> groups;
};
int ExtraStride(uint32_t f)
{
    const int n = f & 1 ? 4 : 0;
    return f & 2 ? n + 8 : f & 4 ? n + 0x10 : f & 8 ? n + 0x18 : n;
}
Writer Mesh(std::vector<S_level> levels, int shared = 0, int numLevels = -1)
{
    return [=](S_bytes &b) {
        b.U16(shared);
        if (shared)
            return;
        b.U8(numLevels >= 0 ? numLevels : static_cast<int>(levels.size()));
        int base = 0;
        for (const S_level &l : levels) {
            b.Floats({l.lod});
            b.U32(l.extraFormat);
            int n = l.vertices;
            if (n == 0xffff) {
                b.U16(0xffff);
                n = base;
                for (int i = 0; i < n; ++i)
                    b.Floats({i * 0.125f, 1.0f - i * 0.0625f});
            } else {
                b.U16(n);
                for (int i = 0; i < n; ++i)
                    b.Floats({(i % 4) - 1.5f, (i / 4) * 0.5f, (i % 3) * 0.25f, 0.0f, 0.6f, 0.8f, i * 0.1f, 1.0f - i * 0.05f});
                if (!base)
                    base = n;
            }
            for (int i = 0; i < ExtraStride(l.extraFormat) * n; ++i)
                b.U8(i * 7 + 3);
            b.U8(static_cast<int>(l.groups.size()));
            for (const auto &g : l.groups) {
                b.U16(g.first);
                for (int k = 0; k < g.first; ++k) {
                    b.U16(k % n);
                    b.U16((k + 1) % n);
                    b.U16((k + 2) % n);
                }
                b.U16(g.second);
            }
        }
    };
}

struct S_frame {
    int type, parent;
    const char *name;
    Writer data;                            // what the type's Load reads after the frame's
    int visual = -1;                        // FRAME_VISUAL: the visual index
    int visualFlags = 0;
    const char *props = "";
};
struct S_case {
    const char *name;
    std::vector<Writer> materials;
    std::vector<S_frame> frames;
};

Writer Box() { return [](S_bytes &b) { b.Floats({-1, -1, -1, 0, 1, 1, 1, 0}); }; }
Writer Target(std::vector<int> frames)
{
    return [=](S_bytes &b) {
        b.U16(1);                           // TARGETF_LOOK_AT
        b.U8(static_cast<int>(frames.size()));
        for (int f : frames)
            b.U16(f);
    };
}
// The light's 0x60 bytes: colors (with w), mode, type, powers, range, falloff, attenuation, cone.
Writer Light(int type, uint32_t mode, float range, float inner, float outer)
{
    return [=](S_bytes &b) {
        b.Floats({1.0f, 0.8f, 0.6f, 0, 0.9f, 0.9f, 1.0f, 0, 0.1f, 0.12f, 0.15f, 0});
        b.U32(mode);
        b.U32(static_cast<uint32_t>(type));
        b.Floats({1.5f, 0.75f, 0.5f, range, 1.0f, type >= 5 ? 20.0f : 1.0f, 0.05f, 0.002f, inner, outer});
    };
}
Writer Projector(int orthogonal, int mode, int material)
{
    return [=](S_bytes &b) {
        b.U8(orthogonal);
        b.U8(mode);
        b.U16(material);
    };
}
Writer Emitter(uint32_t flags, const char *texture, const char *alpha = "")
{
    return [=](S_bytes &b) {
        b.U32(flags);
        b.Name(texture);
        if (*texture && (flags & MATF_ALPHA) && !(flags & 0xa1000000))
            b.Name(alpha);
    };
}

std::vector<S_case> SyntheticCases()
{
    const S_level cube = {0.0f, 0, 8, {{4, 1}, {3, 2}}};
    std::vector<S_case> cases;
    cases.push_back({"lights", {},
                     {{FRAME_DUMMY, 0, "lamps", Box(), -1, 0, "kind=street"},
                      {FRAME_LIGHT, 1, "point", Light(I3DLIGHT_POINT, 0xff6b, 25.0f, 0.5f, 1.0f)},
                      {FRAME_LIGHT, 1, "spot", Light(I3DLIGHT_SPOT, 0x2a, 40.0f, 0.4f, 0.9f)},
                      {FRAME_LIGHT, 0, "sun", Light(I3DLIGHT_DIRECTIONAL, 0x0a, 0.0f, 0.0f, 0.0f)},
                      {FRAME_LIGHT, 0, "ambient", Light(I3DLIGHT_AMBIENT, 0x02, 0.0f, 0.0f, 0.0f)},
                      {FRAME_LIGHT, 0, "fog", Light(I3DLIGHT_FOG_FIRST, 0x00, 300.0f, 0.0f, 0.0f)},
                      {FRAME_CAMERA, 1, "eye", nullptr},
                      {FRAME_SOUND, 1, "hum", nullptr, -1, 0, "loop"},
                      {FRAME_USER, 0, "user", nullptr},
                      {FRAME_VOLUME, 0, "zone", nullptr},
                      {FRAME_SHADOW, 1, "shade", nullptr},
                      {FRAME_MODEL, 0, "inner", nullptr},
                      {FRAME_DUMMY, 12, "innerbox", Box()}}});
    cases.push_back({"visuals",
                     {Material(MATF_COLORS, ""), Material(MATF_TEXTURE, "2ft19r.bmp"),
                      Material(MATF_TEXTURE | MATF_ALPHA, "00.tga", "00a.tga"),
                      Material(MATF_TEXTURE | MATF_COLORKEY | MATF_TWO_SIDED, "2E_CVIT.TGA")},
                     {{FRAME_VISUAL, 0, "box", Mesh({cube, {100.0f, 1, 6, {{2, 3}}}}), 0, 0x800},
                      {FRAME_VISUAL, 1, "lit", Mesh({{0.0f, 0, 12, {{6, 4}, {2, 1}}}}), 1, 0x800},
                      {FRAME_VISUAL, 0, "copy", Mesh({}, 1), 0, 0x800},
                      {FRAME_VISUAL, 0, "texlod", Mesh({{0.0f, 0, 10, {{5, 2}}}, {50.0f, 0, 0xffff, {{5, 3}}}}), 0, 0x810},
                      {FRAME_VISUAL, 0, "extra", Mesh({{0.0f, 3, 7, {{3, 1}}}, {80.0f, 8, 5, {{2, 1}}}}), 0, 0x800},
                      {FRAME_VISUAL, 0, "empty", Mesh({}), 0, 0x800},
                      {FRAME_VISUAL, 1, "proj", Projector(1, 1, 2), 7},
                      {FRAME_VISUAL, 0, "proj2", Projector(0, 0, 0), 7},
                      {FRAME_VISUAL, 0, "smoke", Emitter(MATF_TEXTURE, "2ft19r.bmp"), 9},
                      {FRAME_VISUAL, 9, "spark", Emitter(MATF_TEXTURE | MATF_ALPHA, "00.tga", "00a.tga"), 9},
                      {FRAME_VISUAL, 0, "plain", Emitter(0, ""), 9}}});
    // Scene files (into a scene: the top frames go to its primary sector). An area reads nothing
    // past the frame type and parent (only last in a file), and only at the top of a scene: the
    // original's LinkTo looks up the parents for a sector with a scene (it loops for ever at a
    // sector without one, reads through null above the top frame).
    cases.push_back({"lights_scene\\scene", {},
                     {{FRAME_DUMMY, 0, "lamps", Box()},
                      {FRAME_LIGHT, 1, "point", Light(I3DLIGHT_POINT, 0xff6b, 25.0f, 0.5f, 1.0f)},
                      {FRAME_LIGHT, 0, "spot", Light(I3DLIGHT_SPOT, 0x2a, 40.0f, 0.4f, 0.9f)},
                      {FRAME_LIGHT, 0, "sun", Light(I3DLIGHT_DIRECTIONAL, 0x0a, 0.0f, 0.0f, 0.0f)},
                      {FRAME_LIGHT, 0, "ambient", Light(I3DLIGHT_AMBIENT, 0x02, 0.0f, 0.0f, 0.0f)},
                      {FRAME_LIGHT, 0, "fog", Light(I3DLIGHT_FOG_FIRST + 1, 0x00, 300.0f, 0.0f, 0.0f)},
                      {FRAME_CAMERA, 0, "eye", nullptr},
                      {FRAME_SOUND, 1, "hum", nullptr},
                      {FRAME_USER, 0, "user", nullptr},
                      {FRAME_VOLUME, 0, "zone", nullptr},
                      {FRAME_AREA, 0, "place", nullptr}}});
    cases.push_back({"area_scene\\scene", {}, {{FRAME_DUMMY, 0, "box1", Box()}, {FRAME_AREA, 0, "place", nullptr}}});
    cases.push_back({"targets", {},
                     {{FRAME_DUMMY, 0, "box1", Box()}, {FRAME_DUMMY, 1, "box2", Box()},
                      {FRAME_TARGET, 0, "aim", Target({2, 4})}, {FRAME_DUMMY, 3, "box3", Box()}}});
    cases.push_back({"target_zero", {},
                     {{FRAME_DUMMY, 0, "box1", Box()}, {FRAME_TARGET, 0, "aim", Target({0, 1, 0})},
                      {FRAME_DUMMY, 2, "", Box()}}});
    cases.push_back({"target_past", {},
                     {{FRAME_DUMMY, 0, "box1", Box()}, {FRAME_TARGET, 1, "aim", Target({1, 4})},
                      {FRAME_DUMMY, 0, "box2", Box()}}});
    cases.push_back({"parent_ahead", {},
                     {{FRAME_DUMMY, 0, "box1", Box()}, {FRAME_DUMMY, 3, "box2", Box()}, {FRAME_DUMMY, 1, "box3", Box()}}});
    cases.push_back({"bad_type", {},
                     {{FRAME_DUMMY, 0, "box1", Box()}, {0x11, 1, "x", nullptr}, {FRAME_DUMMY, 1, "box3", Box()}}});
    cases.push_back({"null_type", {}, {{FRAME_DUMMY, 0, "box1", Box()}, {FRAME_NULL, 1, "x", nullptr}}});
    cases.push_back({"land_patch", {}, {{FRAME_DUMMY, 0, "box1", Box()}, {FRAME_VISUAL, 1, "x", nullptr, 10}}});
    cases.push_back({"bad_material", {Material(0, "")},
                     {{FRAME_VISUAL, 0, "box", Mesh({{0.0f, 0, 8, {{2, 1}, {2, 5}, {2, 1}}}}), 0, 0x800}}});
    cases.push_back({"bad_projector", {Material(0, "")}, {{FRAME_VISUAL, 0, "proj", Projector(0, 1, 3), 7}}});
    cases.push_back({"too_many_levels", {}, {{FRAME_VISUAL, 0, "box", Mesh({}, 0, 11), 0, 0x800}}});
    return cases;
}

std::vector<std::string> WriteSyntheticModels(const char *filter)
{
    CreateDirectoryA("Synthetic", nullptr);
    std::vector<std::string> out;
    for (const S_case &c : SyntheticCases()) {
        const std::string name = std::string("Synthetic\\") + c.name + ".4ds";
        if (filter && !std::strstr(name.c_str(), filter))
            continue;
        CreateDirectoryA(name.substr(0, name.rfind('\\')).c_str(), nullptr);
        S_bytes b;
        b.Put("4DS", 4);
        b.U16(0x29);
        b.Put("\0\0\0\0\0\0\0\0", 8);       // (the file time)
        b.U16(static_cast<int>(c.materials.size()));
        for (const Writer &m : c.materials)
            m(b);
        b.U16(static_cast<int>(c.frames.size()));
        for (size_t i = 0; i < c.frames.size(); ++i) {
            const S_frame &f = c.frames[i];
            b.U8(f.type);
            if (f.type == FRAME_VISUAL) {
                b.U8(f.visual);
                if (f.visual == 10)
                    break;                  // (no such visual: the loader stops here)
                b.U16(f.visualFlags);
            }
            b.U16(f.parent);
            if (f.type == FRAME_AREA || f.type == FRAME_NULL || f.type >= 0x11)
                continue;                   // (nothing more read)
            // Position, rotation, scale and scaleW, the flags' low byte, name, properties.
            const float i0 = static_cast<float>(i);
            b.Floats({i0, 0.5f, -2.0f, 0.92388f, 0, 0.38268f, 0, 1, 2, 1, 1});
            b.U8(0x09);
            b.Name(f.name);
            b.Name(f.props);
            if (f.data)
                f.data(b);
        }
        b.U8(0);                            // no animation
        if (FILE *file = std::fopen(name.c_str(), "wb")) {
            std::fwrite(b.v.data(), 1, b.v.size(), file);
            std::fclose(file);
            out.push_back(name);
        }
    }
    return out;
}

bool IsScene(const std::string &name)
{
    return name.size() > 10 && !_stricmp(name.c_str() + name.size() - 10, "\\scene.4ds");
}
bool IsShadow(const std::string &name) { return name.size() > 4 && !_stricmp(name.c_str() + name.size() - 4, ".6ds"); }

LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
{
    std::printf("CRASH (0x%08lx at %p) in %s at %s\n", info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress, g_model.c_str(), Path().c_str());
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(&CrashFilter);
    const char *origPath = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const std::string portPath = argc > 2 && *argv[2] ? argv[2] : ExeDir() + "\\msvc\\LS3DF.dll";
    const long first = argc > 3 ? std::atol(argv[3]) : 0;
    const long count = argc > 4 ? std::atol(argv[4]) : 1000000;
    const char *filter = argc > 5 && *argv[5] ? argv[5] : nullptr;
    g_keepGoing = argc > 6 && !std::strcmp(argv[6], "all");

    // A work folder: the textures' cache files go nowhere (no Maps folder), the port loads from a copy.
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string work = std::string(tmp) + "ls3df_loader";
    CreateDirectoryA(work.c_str(), nullptr);
    const std::string portCopy = work + "\\LS3DF_port.dll";
    const std::string pdb = portPath.substr(0, portPath.size() - 4) + ".pdb";
    if (!CopyFileA(portPath.c_str(), portCopy.c_str(), FALSE) ||
        !CopyFileA(pdb.c_str(), (work + "\\LS3DF.pdb").c_str(), FALSE)) {
        std::printf("cannot copy %s and its PDB to %s (%lu)\n", portPath.c_str(), work.c_str(), GetLastError());
        return 2;
    }
    std::string gameDir = origPath;
    gameDir = gameDir.substr(0, gameDir.find_last_of('\\'));
    SetDllDirectoryA(gameDir.c_str());
    SetCurrentDirectoryA(work.c_str());

    g_side[0].label = "original";
    g_side[1].label = "port";
    g_side[0].dll = LoadLibraryA(origPath);
    g_side[1].dll = LoadLibraryA(portCopy.c_str());
    if (!g_side[0].dll || !g_side[1].dll) {
        std::printf("cannot load the DLLs (%lu)\n", GetLastError());
        return 2;
    }
    g_proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_EXACT_SYMBOLS);
    if (!SymInitialize(g_proc, work.c_str(), FALSE)) {
        std::printf("SymInitialize failed (%lu)\n", GetLastError());
        return 2;
    }
    g_portBase = SymLoadModuleEx(g_proc, nullptr, portCopy.c_str(), "port",
                                 reinterpret_cast<DWORD64>(g_side[1].dll), 0, nullptr, 0);
    IMAGEHLP_MODULE64 info = {};
    info.SizeOfStruct = sizeof info;
    if (!g_portBase || !SymGetModuleInfo64(g_proc, g_portBase, &info) || info.SymType != SymPdb) {
        std::printf("no PDB for the port (%lu)\n", GetLastError());
        return 2;
    }
    for (int s = 0; s < 2; ++s) {
        using GetDriverFn = I3D_driver *(__cdecl *)();
        g_side[s].driver = reinterpret_cast<GetDriverFn>(GetProcAddress(g_side[s].dll, "I3DGetDriver"))();
        Configure(s);
    }
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    MountArchives(gameDir);
    SetUpRules();

    const std::string unpacked = UnpackedDir(gameDir);
    std::vector<std::string> names = ModelNames(unpacked, filter);
    if (names.empty() && !filter)
        std::printf("no game files under %s (set HD2_UNPACKED to the unpacked data)\n", unpacked.c_str());
    const std::vector<std::string> synthetic = WriteSyntheticModels(filter);
    names.insert(names.end(), synthetic.begin(), synthetic.end());
    long loaded = 0, failedLoads = 0, mismatched = 0, done = 0;
    for (long i = first; i < static_cast<long>(names.size()) && done < count; ++i, ++done) {
        g_model = names[i];
        I3D_frame *model[2];
        I3D_RESULT result[2];
        // Every other model is named (its frames' names get the prefix). Scene files load into a
        // scene (slot 0xF4), the rest into a model (0x74).
        // The driver's texture states (SetState, slot 0x18), five combinations in turn: mip maps,
        // dither, 32-bit, half size, compression.
        using namespace ls3d::i3d;
        static const int kStateIds[5] = {DRVSTATE_TEX_MIPMAP, DRVSTATE_TEX_DITHER, DRVSTATE_TEX_TRUECOLOR,
                                         DRVSTATE_TEX_HALF, DRVSTATE_TEX_COMPRESS};
        static const uint32_t kStates[5][5] = {
            {1, 1, 0, 0, 1}, {1, 1, 0, 0, 0}, {1, 1, 1, 0, 0}, {1, 0, 0, 1, 0}, {0, 0, 0, 0, 0}};
        for (int s = 0; s < 2; ++s) {
            for (int k = 0; k < 5; ++k)
                CallV<I3D_RESULT>(g_side[s].driver, 0x18, kStateIds[k], kStates[i % 5][k]);
        }
        // Shadow files into a shadow frame (its slot 0x74: name, flags).
        const bool scene = IsScene(g_model), shadow = IsShadow(g_model);
        const size_t slash = g_model.rfind('\\');
        const std::string base = g_model.substr(slash + 1, g_model.size() - slash - 5);
        const char *kind = scene ? "scene" : shadow ? "shadow" : "model";
        for (int s = 0; s < 2; ++s) {
            model[s] = CallV<I3D_frame *>(g_side[s].driver, 0x54,
                                          static_cast<int>(scene ? FRAME_SCENE : shadow ? FRAME_SHADOW : FRAME_MODEL));
            if ((i & 1) && !scene && !shadow)
                CallV<I3D_RESULT>(model[s], 0x34, base.c_str());
            if (shadow)
                result[s] = CallV<I3D_RESULT>(model[s], 0x74, g_model.c_str(), 0u);
            else
                result[s] = CallV<I3D_RESULT>(model[s], scene ? 0xF4 : 0x74, g_model.c_str(), 0u,
                                              static_cast<void *>(nullptr), static_cast<void *>(nullptr));
        }
        if (result[0] != result[1]) {
            std::printf("MISMATCH %s: Open returns 0x%08lx (original), 0x%08lx (port)\n", g_model.c_str(),
                        static_cast<unsigned long>(result[0]), static_cast<unsigned long>(result[1]));
            ++mismatched;
        } else {
            if (result[0] < 0)
                ++failedLoads;
            else
                ++loaded;
            g_modelFailed = false;
            try {
                WalkFrom(TypeByName(scene ? "I3D_scene" : shadow ? "I3D_shadow" : "I3D_model"),
                         reinterpret_cast<uint8_t *>(model[0]), reinterpret_cast<uint8_t *>(model[1]), kind);
                mismatched += g_modelFailed;
            } catch (const ModelFailure &) {
                ++mismatched;
            }
        }
        for (int s = 0; s < 2; ++s)
            CallV<uint32_t>(model[s], 0x00);
        // What stays loaded after the release.
        for (const S_global *g : {&kCachedTextures, &kUncachedTextures}) {
            const int a = Field<int>(&Global<uint8_t>(0, *g), 8), b = Field<int>(&Global<uint8_t>(1, *g), 8);
            if (a != b) {
                std::printf("MISMATCH %s: after the release %s holds %d textures (original), %d (port)\n",
                            g_model.c_str(), g->name, a, b);
                ++mismatched;
            }
        }
        if (done % 100 == 99)
            std::printf("... %ld models\n", done + 1);
    }

    std::printf("%ld models: %ld loaded, %ld failed to load on both sides, %ld mismatched\n", done, loaded,
                failedLoads, mismatched);
    std::printf("objects compared:");
    for (const auto &c : g_classes)
        std::printf(" %s %ld,", c.first.c_str(), c.second);
    std::printf("\ntargets' frames: %ld\ntextures by format:", g_targetFrames);
    for (const auto &f : g_formats) {
        const char *n = reinterpret_cast<const char *>(&f.first);
        if (f.first > 0xffff)
            std::printf(" %.4s %ld,", n, f.second);
        else
            std::printf(" %d %ld,", f.first, f.second);
    }
    std::printf("\n");
    for (const auto &u : g_plainTargets)
        std::printf("pointer to a plain struct, its target compared as one object: %s x%ld\n", u.first.c_str(), u.second);
    for (const auto &u : g_unhandled)
        std::printf("not compared (pointer target unknown): %s x%ld\n", u.first.c_str(), u.second);
    for (const auto &u : g_overlapped)
        std::printf("not compared (union member): %s x%ld\n", u.first.c_str(), u.second);
    for (const auto &u : g_notValid)
        std::printf("not compared while its valid flag is clear: %s x%ld\n", u.first.c_str(), u.second);
    for (const auto &m : g_soft)
        std::printf("unset member differs (not a failure): %s x%ld (the original's 0: %ld), first: %s\n",
                    m.first.c_str(), m.second.count, m.second.originalZero, m.second.example.c_str());
    for (const auto &m : g_mismatches)
        std::printf("mismatches in %s: %ld, first: %s\n", m.first.c_str(), m.second.count, m.second.example.c_str());
    return mismatched ? 1 : 0;
}
