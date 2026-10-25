// Direct3D 8 API declarations (the interfaces in vtable order and the types LS3D uses).
//
// The Windows SDK no longer ships the Direct3D 8 headers; d3d8.dll still exports Direct3DCreate8
// and implements these interfaces. Written from the published Direct3D 8 ABI.
#pragma once

#include <windows.h>
#include <objbase.h>

#define D3D_SDK_VERSION 220

// ---------------------------------------------------------------- basic types

// (Guarded like the SDK headers: dsound.h declares D3DCOLOR and D3DVECTOR too.)
#ifndef D3DCOLOR_DEFINED
typedef DWORD D3DCOLOR;
#define D3DCOLOR_DEFINED
#endif

#define D3DCOLOR_ARGB(a, r, g, b) \
    ((D3DCOLOR)((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))

#ifndef D3DVECTOR_DEFINED
typedef struct _D3DVECTOR {
    float x, y, z;
} D3DVECTOR;
#define D3DVECTOR_DEFINED
#endif

struct D3DCOLORVALUE {
    float r, g, b, a;
};

struct D3DRECT {
    LONG x1, y1, x2, y2;
};

struct D3DMATRIX {
    float m[4][4];
};

struct D3DVIEWPORT8 {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};

struct D3DCLIPSTATUS8 {
    DWORD ClipUnion, ClipIntersection;
};

struct D3DMATERIAL8 {
    D3DCOLORVALUE Diffuse, Ambient, Specular, Emissive;
    float Power;
};

enum D3DLIGHTTYPE {
    D3DLIGHT_POINT = 1,
    D3DLIGHT_SPOT = 2,
    D3DLIGHT_DIRECTIONAL = 3,
    D3DLIGHT_FORCE_DWORD = 0x7fffffff
};

struct D3DLIGHT8 {
    D3DLIGHTTYPE Type;
    D3DCOLORVALUE Diffuse, Specular, Ambient;
    D3DVECTOR Position, Direction;
    float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
};

#define D3DCLEAR_TARGET 0x00000001L
#define D3DCLEAR_ZBUFFER 0x00000002L
#define D3DCLEAR_STENCIL 0x00000004L

enum D3DSHADEMODE { D3DSHADE_FLAT = 1, D3DSHADE_GOURAUD = 2, D3DSHADE_PHONG = 3, D3DSHADE_FORCE_DWORD = 0x7fffffff };
enum D3DFILLMODE { D3DFILL_POINT = 1, D3DFILL_WIREFRAME = 2, D3DFILL_SOLID = 3, D3DFILL_FORCE_DWORD = 0x7fffffff };

enum D3DBLEND {
    D3DBLEND_ZERO = 1, D3DBLEND_ONE = 2, D3DBLEND_SRCCOLOR = 3, D3DBLEND_INVSRCCOLOR = 4, D3DBLEND_SRCALPHA = 5,
    D3DBLEND_INVSRCALPHA = 6, D3DBLEND_DESTALPHA = 7, D3DBLEND_INVDESTALPHA = 8, D3DBLEND_DESTCOLOR = 9,
    D3DBLEND_INVDESTCOLOR = 10, D3DBLEND_SRCALPHASAT = 11, D3DBLEND_BOTHSRCALPHA = 12,
    D3DBLEND_BOTHINVSRCALPHA = 13, D3DBLEND_FORCE_DWORD = 0x7fffffff
};

enum D3DBLENDOP {
    D3DBLENDOP_ADD = 1, D3DBLENDOP_SUBTRACT = 2, D3DBLENDOP_REVSUBTRACT = 3, D3DBLENDOP_MIN = 4,
    D3DBLENDOP_MAX = 5, D3DBLENDOP_FORCE_DWORD = 0x7fffffff
};

enum D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP = 1, D3DTADDRESS_MIRROR = 2, D3DTADDRESS_CLAMP = 3, D3DTADDRESS_BORDER = 4,
    D3DTADDRESS_MIRRORONCE = 5, D3DTADDRESS_FORCE_DWORD = 0x7fffffff
};

enum D3DCULL { D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3, D3DCULL_FORCE_DWORD = 0x7fffffff };

enum D3DCMPFUNC {
    D3DCMP_NEVER = 1, D3DCMP_LESS = 2, D3DCMP_EQUAL = 3, D3DCMP_LESSEQUAL = 4, D3DCMP_GREATER = 5,
    D3DCMP_NOTEQUAL = 6, D3DCMP_GREATEREQUAL = 7, D3DCMP_ALWAYS = 8, D3DCMP_FORCE_DWORD = 0x7fffffff
};

enum D3DSTENCILOP {
    D3DSTENCILOP_KEEP = 1, D3DSTENCILOP_ZERO = 2, D3DSTENCILOP_REPLACE = 3, D3DSTENCILOP_INCRSAT = 4,
    D3DSTENCILOP_DECRSAT = 5, D3DSTENCILOP_INVERT = 6, D3DSTENCILOP_INCR = 7, D3DSTENCILOP_DECR = 8,
    D3DSTENCILOP_FORCE_DWORD = 0x7fffffff
};

enum D3DFOGMODE { D3DFOG_NONE = 0, D3DFOG_EXP = 1, D3DFOG_EXP2 = 2, D3DFOG_LINEAR = 3, D3DFOG_FORCE_DWORD = 0x7fffffff };
enum D3DZBUFFERTYPE { D3DZB_FALSE = 0, D3DZB_TRUE = 1, D3DZB_USEW = 2, D3DZB_FORCE_DWORD = 0x7fffffff };

enum D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3, D3DPT_TRIANGLELIST = 4,
    D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6, D3DPT_FORCE_DWORD = 0x7fffffff
};

enum D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_TEXTURE0 = 16, D3DTS_TEXTURE1 = 17, D3DTS_TEXTURE2 = 18,
    D3DTS_TEXTURE3 = 19, D3DTS_TEXTURE4 = 20, D3DTS_TEXTURE5 = 21, D3DTS_TEXTURE6 = 22, D3DTS_TEXTURE7 = 23,
    D3DTS_FORCE_DWORD = 0x7fffffff
};
#define D3DTS_WORLDMATRIX(index) (D3DTRANSFORMSTATETYPE)((index) + 256)
#define D3DTS_WORLD D3DTS_WORLDMATRIX(0)

enum D3DRENDERSTATETYPE {
    D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_SHADEMODE = 9, D3DRS_LINEPATTERN = 10,
    D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_LASTPIXEL = 16, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_DITHERENABLE = 26, D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_SPECULARENABLE = 29,
    D3DRS_ZVISIBLE = 30, D3DRS_FOGCOLOR = 34, D3DRS_FOGTABLEMODE = 35, D3DRS_FOGSTART = 36, D3DRS_FOGEND = 37,
    D3DRS_FOGDENSITY = 38, D3DRS_EDGEANTIALIAS = 40, D3DRS_ZBIAS = 47, D3DRS_RANGEFOGENABLE = 48,
    D3DRS_STENCILENABLE = 52, D3DRS_STENCILFAIL = 53, D3DRS_STENCILZFAIL = 54, D3DRS_STENCILPASS = 55,
    D3DRS_STENCILFUNC = 56, D3DRS_STENCILREF = 57, D3DRS_STENCILMASK = 58, D3DRS_STENCILWRITEMASK = 59,
    D3DRS_TEXTUREFACTOR = 60, D3DRS_WRAP0 = 128, D3DRS_WRAP1 = 129, D3DRS_WRAP2 = 130, D3DRS_WRAP3 = 131,
    D3DRS_WRAP4 = 132, D3DRS_WRAP5 = 133, D3DRS_WRAP6 = 134, D3DRS_WRAP7 = 135, D3DRS_CLIPPING = 136,
    D3DRS_LIGHTING = 137, D3DRS_AMBIENT = 139, D3DRS_FOGVERTEXMODE = 140, D3DRS_COLORVERTEX = 141,
    D3DRS_LOCALVIEWER = 142, D3DRS_NORMALIZENORMALS = 143, D3DRS_DIFFUSEMATERIALSOURCE = 145,
    D3DRS_SPECULARMATERIALSOURCE = 146, D3DRS_AMBIENTMATERIALSOURCE = 147, D3DRS_EMISSIVEMATERIALSOURCE = 148,
    D3DRS_VERTEXBLEND = 151, D3DRS_CLIPPLANEENABLE = 152, D3DRS_SOFTWAREVERTEXPROCESSING = 153,
    D3DRS_POINTSIZE = 154, D3DRS_POINTSIZE_MIN = 155, D3DRS_POINTSPRITEENABLE = 156,
    D3DRS_POINTSCALEENABLE = 157, D3DRS_POINTSCALE_A = 158, D3DRS_POINTSCALE_B = 159, D3DRS_POINTSCALE_C = 160,
    D3DRS_MULTISAMPLEANTIALIAS = 161, D3DRS_MULTISAMPLEMASK = 162, D3DRS_PATCHEDGESTYLE = 163,
    D3DRS_PATCHSEGMENTS = 164, D3DRS_DEBUGMONITORTOKEN = 165, D3DRS_POINTSIZE_MAX = 166,
    D3DRS_INDEXEDVERTEXBLENDENABLE = 167, D3DRS_COLORWRITEENABLE = 168, D3DRS_TWEENFACTOR = 170,
    D3DRS_BLENDOP = 171, D3DRS_POSITIONORDER = 172, D3DRS_NORMALORDER = 173, D3DRS_FORCE_DWORD = 0x7fffffff
};

enum D3DMATERIALCOLORSOURCE {
    D3DMCS_MATERIAL = 0, D3DMCS_COLOR1 = 1, D3DMCS_COLOR2 = 2, D3DMCS_FORCE_DWORD = 0x7fffffff
};

#define D3DWRAP_U 0x00000001L
#define D3DWRAP_V 0x00000002L
#define D3DWRAP_W 0x00000004L

#define D3DCOLORWRITEENABLE_RED (1L << 0)
#define D3DCOLORWRITEENABLE_GREEN (1L << 1)
#define D3DCOLORWRITEENABLE_BLUE (1L << 2)
#define D3DCOLORWRITEENABLE_ALPHA (1L << 3)

enum D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6, D3DTSS_BUMPENVMAT00 = 7, D3DTSS_BUMPENVMAT01 = 8, D3DTSS_BUMPENVMAT10 = 9,
    D3DTSS_BUMPENVMAT11 = 10, D3DTSS_TEXCOORDINDEX = 11, D3DTSS_ADDRESSU = 13, D3DTSS_ADDRESSV = 14,
    D3DTSS_BORDERCOLOR = 15, D3DTSS_MAGFILTER = 16, D3DTSS_MINFILTER = 17, D3DTSS_MIPFILTER = 18,
    D3DTSS_MIPMAPLODBIAS = 19, D3DTSS_MAXMIPLEVEL = 20, D3DTSS_MAXANISOTROPY = 21, D3DTSS_BUMPENVLSCALE = 22,
    D3DTSS_BUMPENVLOFFSET = 23, D3DTSS_TEXTURETRANSFORMFLAGS = 24, D3DTSS_ADDRESSW = 25, D3DTSS_COLORARG0 = 26,
    D3DTSS_ALPHAARG0 = 27, D3DTSS_RESULTARG = 28, D3DTSS_FORCE_DWORD = 0x7fffffff
};

#define D3DTSS_TCI_PASSTHRU 0x00000000
#define D3DTSS_TCI_CAMERASPACENORMAL 0x00010000
#define D3DTSS_TCI_CAMERASPACEPOSITION 0x00020000
#define D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR 0x00030000

enum D3DTEXTUREOP {
    D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4, D3DTOP_MODULATE2X = 5,
    D3DTOP_MODULATE4X = 6, D3DTOP_ADD = 7, D3DTOP_ADDSIGNED = 8, D3DTOP_ADDSIGNED2X = 9, D3DTOP_SUBTRACT = 10,
    D3DTOP_ADDSMOOTH = 11, D3DTOP_BLENDDIFFUSEALPHA = 12, D3DTOP_BLENDTEXTUREALPHA = 13,
    D3DTOP_BLENDFACTORALPHA = 14, D3DTOP_BLENDTEXTUREALPHAPM = 15, D3DTOP_BLENDCURRENTALPHA = 16,
    D3DTOP_PREMODULATE = 17, D3DTOP_MODULATEALPHA_ADDCOLOR = 18, D3DTOP_MODULATECOLOR_ADDALPHA = 19,
    D3DTOP_MODULATEINVALPHA_ADDCOLOR = 20, D3DTOP_MODULATEINVCOLOR_ADDALPHA = 21, D3DTOP_BUMPENVMAP = 22,
    D3DTOP_BUMPENVMAPLUMINANCE = 23, D3DTOP_DOTPRODUCT3 = 24, D3DTOP_MULTIPLYADD = 25, D3DTOP_LERP = 26,
    D3DTOP_FORCE_DWORD = 0x7fffffff
};

#define D3DTA_SELECTMASK 0x0000000f
#define D3DTA_DIFFUSE 0x00000000
#define D3DTA_CURRENT 0x00000001
#define D3DTA_TEXTURE 0x00000002
#define D3DTA_TFACTOR 0x00000003
#define D3DTA_SPECULAR 0x00000004
#define D3DTA_TEMP 0x00000005
#define D3DTA_COMPLEMENT 0x00000010
#define D3DTA_ALPHAREPLICATE 0x00000020

enum D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2, D3DTEXF_ANISOTROPIC = 3, D3DTEXF_FLATCUBIC = 4,
    D3DTEXF_GAUSSIANCUBIC = 5, D3DTEXF_FORCE_DWORD = 0x7fffffff
};

enum D3DTEXTURETRANSFORMFLAGS {
    D3DTTFF_DISABLE = 0, D3DTTFF_COUNT1 = 1, D3DTTFF_COUNT2 = 2, D3DTTFF_COUNT3 = 3, D3DTTFF_COUNT4 = 4,
    D3DTTFF_PROJECTED = 256, D3DTTFF_FORCE_DWORD = 0x7fffffff
};

enum D3DVERTEXBLENDFLAGS {
    D3DVBF_DISABLE = 0, D3DVBF_1WEIGHTS = 1, D3DVBF_2WEIGHTS = 2, D3DVBF_3WEIGHTS = 3, D3DVBF_TWEENING = 255,
    D3DVBF_0WEIGHTS = 256, D3DVBF_FORCE_DWORD = 0x7fffffff
};

// Flexible vertex formats
#define D3DFVF_RESERVED0 0x001
#define D3DFVF_POSITION_MASK 0x00E
#define D3DFVF_XYZ 0x002
#define D3DFVF_XYZRHW 0x004
#define D3DFVF_XYZB1 0x006
#define D3DFVF_XYZB2 0x008
#define D3DFVF_XYZB3 0x00a
#define D3DFVF_XYZB4 0x00c
#define D3DFVF_XYZB5 0x00e
#define D3DFVF_NORMAL 0x010
#define D3DFVF_PSIZE 0x020
#define D3DFVF_DIFFUSE 0x040
#define D3DFVF_SPECULAR 0x080
#define D3DFVF_TEXCOUNT_MASK 0xf00
#define D3DFVF_TEXCOUNT_SHIFT 8
#define D3DFVF_TEX0 0x000
#define D3DFVF_TEX1 0x100
#define D3DFVF_TEX2 0x200
#define D3DFVF_TEX3 0x300
#define D3DFVF_TEX4 0x400
#define D3DFVF_LASTBETA_UBYTE4 0x1000

// Vertex shader declaration tokens
enum D3DVSD_TOKENTYPE {
    D3DVSD_TOKEN_NOP = 0, D3DVSD_TOKEN_STREAM = 1, D3DVSD_TOKEN_STREAMDATA = 2, D3DVSD_TOKEN_TESSELLATOR = 3,
    D3DVSD_TOKEN_CONSTMEM = 4, D3DVSD_TOKEN_EXT = 5, D3DVSD_TOKEN_END = 7, D3DVSD_FORCE_DWORD = 0x7fffffff
};
#define D3DVSD_TOKENTYPESHIFT 29
#define D3DVSD_STREAMNUMBERSHIFT 0
#define D3DVSD_DATALOADTYPESHIFT 28
#define D3DVSD_DATATYPESHIFT 16
#define D3DVSD_SKIPCOUNTSHIFT 16
#define D3DVSD_VERTEXREGSHIFT 0
#define D3DVSD_CONSTCOUNTSHIFT 25
#define D3DVSD_CONSTADDRESSSHIFT 0
#define D3DVSD_MAKETOKENTYPE(tokenType) ((tokenType << D3DVSD_TOKENTYPESHIFT) & 0xE0000000)
#define D3DVSD_STREAM(StreamNumber) (D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_STREAM) | (StreamNumber))
#define D3DVSD_REG(VertexRegister, Type) \
    (D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_STREAMDATA) | ((Type) << D3DVSD_DATATYPESHIFT) | (VertexRegister))
#define D3DVSD_SKIP(DWORDCount) \
    (D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_STREAMDATA) | 0x10000000 | ((DWORDCount) << D3DVSD_SKIPCOUNTSHIFT))
#define D3DVSD_CONST(ConstantAddress, Count) \
    (D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_CONSTMEM) | ((Count) << D3DVSD_CONSTCOUNTSHIFT) | (ConstantAddress))
#define D3DVSD_END() 0xFFFFFFFF
#define D3DVSD_NOP() 0x00000000
#define D3DVSDT_FLOAT1 0x00
#define D3DVSDT_FLOAT2 0x01
#define D3DVSDT_FLOAT3 0x02
#define D3DVSDT_FLOAT4 0x03
#define D3DVSDT_D3DCOLOR 0x04
#define D3DVSDT_UBYTE4 0x05
#define D3DVSDT_SHORT2 0x06
#define D3DVSDT_SHORT4 0x07

enum D3DBASISTYPE { D3DBASIS_BEZIER = 0, D3DBASIS_BSPLINE = 1, D3DBASIS_INTERPOLATE = 2, D3DBASIS_FORCE_DWORD = 0x7fffffff };
enum D3DORDERTYPE { D3DORDER_LINEAR = 1, D3DORDER_CUBIC = 3, D3DORDER_QUINTIC = 5, D3DORDER_FORCE_DWORD = 0x7fffffff };
enum D3DPATCHEDGESTYLE { D3DPATCHEDGE_DISCRETE = 0, D3DPATCHEDGE_CONTINUOUS = 1, D3DPATCHEDGE_FORCE_DWORD = 0x7fffffff };

struct D3DRECTPATCH_INFO {
    UINT StartVertexOffsetWidth, StartVertexOffsetHeight, Width, Height, Stride;
    D3DBASISTYPE Basis;
    D3DORDERTYPE Order;
};

struct D3DTRIPATCH_INFO {
    UINT StartVertexOffset, NumVertices;
    D3DBASISTYPE Basis;
    D3DORDERTYPE Order;
};

enum D3DSTATEBLOCKTYPE {
    D3DSBT_ALL = 1, D3DSBT_PIXELSTATE = 2, D3DSBT_VERTEXSTATE = 3, D3DSBT_FORCE_DWORD = 0x7fffffff
};

enum D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD = 1, D3DSWAPEFFECT_FLIP = 2, D3DSWAPEFFECT_COPY = 3, D3DSWAPEFFECT_COPY_VSYNC = 4,
    D3DSWAPEFFECT_FORCE_DWORD = 0x7fffffff
};

enum D3DPOOL {
    D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3,
    D3DPOOL_FORCE_DWORD = 0x7fffffff
};

#define D3DPRESENT_RATE_DEFAULT 0x00000000
#define D3DPRESENT_RATE_UNLIMITED 0x7fffffff
#define D3DPRESENT_INTERVAL_DEFAULT 0x00000000
#define D3DPRESENT_INTERVAL_ONE 0x00000001
#define D3DPRESENT_INTERVAL_TWO 0x00000002
#define D3DPRESENT_INTERVAL_THREE 0x00000004
#define D3DPRESENT_INTERVAL_FOUR 0x00000008
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000L
#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 0x00000001

enum D3DBACKBUFFER_TYPE { D3DBACKBUFFER_TYPE_MONO = 0, D3DBACKBUFFER_TYPE_FORCE_DWORD = 0x7fffffff };

enum D3DRESOURCETYPE {
    D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3, D3DRTYPE_VOLUMETEXTURE = 4,
    D3DRTYPE_CUBETEXTURE = 5, D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7, D3DRTYPE_FORCE_DWORD = 0x7fffffff
};

#define D3DUSAGE_RENDERTARGET 0x00000001L
#define D3DUSAGE_DEPTHSTENCIL 0x00000002L
#define D3DUSAGE_WRITEONLY 0x00000008L
#define D3DUSAGE_SOFTWAREPROCESSING 0x00000010L
#define D3DUSAGE_DONOTCLIP 0x00000020L
#define D3DUSAGE_POINTS 0x00000040L
#define D3DUSAGE_RTPATCHES 0x00000080L
#define D3DUSAGE_NPATCHES 0x00000100L
#define D3DUSAGE_DYNAMIC 0x00000200L

enum D3DCUBEMAP_FACES {
    D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X = 1, D3DCUBEMAP_FACE_POSITIVE_Y = 2,
    D3DCUBEMAP_FACE_NEGATIVE_Y = 3, D3DCUBEMAP_FACE_POSITIVE_Z = 4, D3DCUBEMAP_FACE_NEGATIVE_Z = 5,
    D3DCUBEMAP_FACE_FORCE_DWORD = 0x7fffffff
};

#define D3DLOCK_READONLY 0x00000010L
#define D3DLOCK_DISCARD 0x00002000L
#define D3DLOCK_NOOVERWRITE 0x00001000L
#define D3DLOCK_NOSYSLOCK 0x00000800L
#define D3DLOCK_NO_DIRTY_UPDATE 0x00008000L

enum D3DDEVTYPE { D3DDEVTYPE_HAL = 1, D3DDEVTYPE_REF = 2, D3DDEVTYPE_SW = 3, D3DDEVTYPE_FORCE_DWORD = 0x7fffffff };

enum D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0, D3DMULTISAMPLE_2_SAMPLES = 2, D3DMULTISAMPLE_3_SAMPLES = 3,
    D3DMULTISAMPLE_4_SAMPLES = 4, D3DMULTISAMPLE_5_SAMPLES = 5, D3DMULTISAMPLE_6_SAMPLES = 6,
    D3DMULTISAMPLE_7_SAMPLES = 7, D3DMULTISAMPLE_8_SAMPLES = 8, D3DMULTISAMPLE_9_SAMPLES = 9,
    D3DMULTISAMPLE_10_SAMPLES = 10, D3DMULTISAMPLE_11_SAMPLES = 11, D3DMULTISAMPLE_12_SAMPLES = 12,
    D3DMULTISAMPLE_13_SAMPLES = 13, D3DMULTISAMPLE_14_SAMPLES = 14, D3DMULTISAMPLE_15_SAMPLES = 15,
    D3DMULTISAMPLE_16_SAMPLES = 16, D3DMULTISAMPLE_FORCE_DWORD = 0x7fffffff
};

#define MAKEFOURCC_D3D(ch0, ch1, ch2, ch3) \
    ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))

enum D3DFORMAT {
    D3DFMT_UNKNOWN = 0,
    D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23, D3DFMT_X1R5G5B5 = 24,
    D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27, D3DFMT_A8 = 28, D3DFMT_A8R3G3B2 = 29,
    D3DFMT_X4R4G4B4 = 30, D3DFMT_A2B10G10R10 = 31, D3DFMT_G16R16 = 34,
    D3DFMT_A8P8 = 40, D3DFMT_P8 = 41,
    D3DFMT_L8 = 50, D3DFMT_A8L8 = 51, D3DFMT_A4L4 = 52,
    D3DFMT_V8U8 = 60, D3DFMT_L6V5U5 = 61, D3DFMT_X8L8V8U8 = 62, D3DFMT_Q8W8V8U8 = 63, D3DFMT_V16U16 = 64,
    D3DFMT_W11V11U10 = 65, D3DFMT_A2W10V10U10 = 67,
    D3DFMT_UYVY = MAKEFOURCC_D3D('U', 'Y', 'V', 'Y'),
    D3DFMT_YUY2 = MAKEFOURCC_D3D('Y', 'U', 'Y', '2'),
    D3DFMT_DXT1 = MAKEFOURCC_D3D('D', 'X', 'T', '1'),
    D3DFMT_DXT2 = MAKEFOURCC_D3D('D', 'X', 'T', '2'),
    D3DFMT_DXT3 = MAKEFOURCC_D3D('D', 'X', 'T', '3'),
    D3DFMT_DXT4 = MAKEFOURCC_D3D('D', 'X', 'T', '4'),
    D3DFMT_DXT5 = MAKEFOURCC_D3D('D', 'X', 'T', '5'),
    D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71, D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D16 = 80,
    D3DFMT_D24X8 = 77, D3DFMT_D24X4S4 = 79,
    D3DFMT_VERTEXDATA = 100, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
    D3DFMT_FORCE_DWORD = 0x7fffffff
};

struct D3DDISPLAYMODE {
    UINT Width, Height, RefreshRate;
    D3DFORMAT Format;
};

struct D3DDEVICE_CREATION_PARAMETERS {
    UINT AdapterOrdinal;
    D3DDEVTYPE DeviceType;
    HWND hFocusWindow;
    DWORD BehaviorFlags;
};

struct D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

struct D3DGAMMARAMP {
    WORD red[256], green[256], blue[256];
};

struct D3DSURFACE_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width, Height;
};

struct D3DVOLUME_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size, Width, Height, Depth;
};

struct D3DVERTEXBUFFER_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    DWORD FVF;
};

struct D3DINDEXBUFFER_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
};

struct D3DLOCKED_RECT {
    INT Pitch;
    void *pBits;
};

struct D3DBOX {
    UINT Left, Top, Right, Bottom, Front, Back;
};

struct D3DLOCKED_BOX {
    INT RowPitch, SlicePitch;
    void *pBits;
};

struct D3DRASTER_STATUS {
    BOOL InVBlank;
    UINT ScanLine;
};

#define MAX_DEVICE_IDENTIFIER_STRING 512
struct D3DADAPTER_IDENTIFIER8 {
    char Driver[MAX_DEVICE_IDENTIFIER_STRING];
    char Description[MAX_DEVICE_IDENTIFIER_STRING];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
};

#define D3DENUM_NO_WHQL_LEVEL 0x00000002L
#define D3DADAPTER_DEFAULT 0

#define D3DCREATE_FPU_PRESERVE 0x00000002L
#define D3DCREATE_MULTITHREADED 0x00000004L
#define D3DCREATE_PUREDEVICE 0x00000010L
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020L
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040L
#define D3DCREATE_MIXED_VERTEXPROCESSING 0x00000080L

#define D3DSGR_NO_CALIBRATION 0x00000000L
#define D3DSGR_CALIBRATE 0x00000001L

// ---------------------------------------------------------------- caps

struct D3DCAPS8 {
    D3DDEVTYPE DeviceType;
    UINT AdapterOrdinal;
    DWORD Caps, Caps2, Caps3, PresentationIntervals;
    DWORD CursorCaps;
    DWORD DevCaps;
    DWORD PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps,
        TextureCaps, TextureFilterCaps, CubeTextureFilterCaps, VolumeTextureFilterCaps, TextureAddressCaps,
        VolumeTextureAddressCaps;
    DWORD LineCaps;
    DWORD MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent;
    DWORD MaxTextureRepeat, MaxTextureAspectRatio, MaxAnisotropy;
    float MaxVertexW;
    float GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom;
    float ExtentsAdjust;
    DWORD StencilCaps;
    DWORD FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures;
    DWORD VertexProcessingCaps, MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices,
        MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride;
    DWORD VertexShaderVersion, MaxVertexShaderConst;
    DWORD PixelShaderVersion;
    float MaxPixelShaderValue;
};
static_assert(sizeof(D3DCAPS8) == 0xD4, "D3DCAPS8 layout");

#define D3DCAPS2_FULLSCREENGAMMA 0x00020000L
#define D3DCAPS2_CANCALIBRATEGAMMA 0x00100000L
#define D3DDEVCAPS_HWTRANSFORMANDLIGHT 0x00010000L
#define D3DPTFILTERCAPS_MINFANISOTROPIC 0x00000400L

#define D3DTEXOPCAPS_DISABLE 0x00000001L
#define D3DTEXOPCAPS_SELECTARG1 0x00000002L
#define D3DTEXOPCAPS_SELECTARG2 0x00000004L
#define D3DTEXOPCAPS_MODULATE 0x00000008L
#define D3DTEXOPCAPS_MODULATE2X 0x00000010L
#define D3DTEXOPCAPS_MODULATE4X 0x00000020L
#define D3DTEXOPCAPS_ADD 0x00000040L
#define D3DTEXOPCAPS_ADDSIGNED 0x00000080L
#define D3DTEXOPCAPS_ADDSIGNED2X 0x00000100L
#define D3DTEXOPCAPS_SUBTRACT 0x00000200L
#define D3DTEXOPCAPS_ADDSMOOTH 0x00000400L
#define D3DTEXOPCAPS_BLENDDIFFUSEALPHA 0x00000800L
#define D3DTEXOPCAPS_BLENDTEXTUREALPHA 0x00001000L
#define D3DTEXOPCAPS_BLENDFACTORALPHA 0x00002000L
#define D3DTEXOPCAPS_BLENDTEXTUREALPHAPM 0x00004000L
#define D3DTEXOPCAPS_BLENDCURRENTALPHA 0x00008000L
#define D3DTEXOPCAPS_PREMODULATE 0x00010000L
#define D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR 0x00020000L
#define D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA 0x00040000L
#define D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR 0x00080000L
#define D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA 0x00100000L
#define D3DTEXOPCAPS_BUMPENVMAP 0x00200000L
#define D3DTEXOPCAPS_BUMPENVMAPLUMINANCE 0x00400000L
#define D3DTEXOPCAPS_DOTPRODUCT3 0x00800000L
#define D3DTEXOPCAPS_MULTIPLYADD 0x01000000L
#define D3DTEXOPCAPS_LERP 0x02000000L
#define D3DDEVCAPS_PUREDEVICE 0x00100000L
#define D3DPRASTERCAPS_FOGVERTEX 0x00000080L
#define D3DPRASTERCAPS_FOGTABLE 0x00000100L
#define D3DPRASTERCAPS_WFOG 0x00100000L
#define D3DPTEXTURECAPS_POW2 0x00000002L
#define D3DPTEXTURECAPS_SQUAREONLY 0x00000020L
#define D3DPTEXTURECAPS_CUBEMAP 0x00000800L
#define D3DTEXOPCAPS_DOTPRODUCT3 0x00800000L
#define D3DTEXOPCAPS_BUMPENVMAP 0x00200000L
#define D3DTEXOPCAPS_BUMPENVMAPLUMINANCE 0x00400000L
#define D3DVS_VERSION(major, minor) (0xFFFE0000 | ((major) << 8) | (minor))
#define D3DPS_VERSION(major, minor) (0xFFFF0000 | ((major) << 8) | (minor))

// ---------------------------------------------------------------- interfaces

struct IDirect3D8;
struct IDirect3DDevice8;
struct IDirect3DResource8;
struct IDirect3DBaseTexture8;
struct IDirect3DTexture8;
struct IDirect3DVolumeTexture8;
struct IDirect3DCubeTexture8;
struct IDirect3DVertexBuffer8;
struct IDirect3DIndexBuffer8;
struct IDirect3DSurface8;
struct IDirect3DVolume8;
struct IDirect3DSwapChain8;

struct IDirect3D8 : public IUnknown {
    virtual HRESULT __stdcall RegisterSoftwareDevice(void *pInitializeFunction) = 0;
    virtual UINT __stdcall GetAdapterCount() = 0;
    virtual HRESULT __stdcall GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 *pIdentifier) = 0;
    virtual UINT __stdcall GetAdapterModeCount(UINT Adapter) = 0;
    virtual HRESULT __stdcall EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE *pMode) = 0;
    virtual HRESULT __stdcall GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) = 0;
    virtual HRESULT __stdcall CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat,
                                              D3DFORMAT BackBufferFormat, BOOL Windowed) = 0;
    virtual HRESULT __stdcall CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                                                DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) = 0;
    virtual HRESULT __stdcall CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType,
                                                         D3DFORMAT SurfaceFormat, BOOL Windowed,
                                                         D3DMULTISAMPLE_TYPE MultiSampleType) = 0;
    virtual HRESULT __stdcall CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                                                     D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) = 0;
    virtual HRESULT __stdcall GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 *pCaps) = 0;
    virtual HMONITOR __stdcall GetAdapterMonitor(UINT Adapter) = 0;
    virtual HRESULT __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                           DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters,
                                           IDirect3DDevice8 **ppReturnedDeviceInterface) = 0;
};

struct IDirect3DDevice8 : public IUnknown {
    virtual HRESULT __stdcall TestCooperativeLevel() = 0;                                   // 0x0C
    virtual UINT __stdcall GetAvailableTextureMem() = 0;
    virtual HRESULT __stdcall ResourceManagerDiscardBytes(DWORD Bytes) = 0;
    virtual HRESULT __stdcall GetDirect3D(IDirect3D8 **ppD3D8) = 0;
    virtual HRESULT __stdcall GetDeviceCaps(D3DCAPS8 *pCaps) = 0;                           // 0x1C
    virtual HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE *pMode) = 0;
    virtual HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) = 0;
    virtual HRESULT __stdcall SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap) = 0;
    virtual void __stdcall SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags) = 0;   // 0x2C
    virtual BOOL __stdcall ShowCursor(BOOL bShow) = 0;
    virtual HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters,
                                                        IDirect3DSwapChain8 **pSwapChain) = 0;
    virtual HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) = 0;    // 0x38
    virtual HRESULT __stdcall Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride,
                                      const RGNDATA *pDirtyRegion) = 0;                     // 0x3C
    virtual HRESULT __stdcall GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type,
                                            IDirect3DSurface8 **ppBackBuffer) = 0;
    virtual HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) = 0;
    virtual void __stdcall SetGammaRamp(DWORD Flags, const D3DGAMMARAMP *pRamp) = 0;        // 0x48
    virtual void __stdcall GetGammaRamp(D3DGAMMARAMP *pRamp) = 0;
    virtual HRESULT __stdcall CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                            D3DPOOL Pool, IDirect3DTexture8 **ppTexture) = 0;   // 0x50
    virtual HRESULT __stdcall CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                                  D3DFORMAT Format, D3DPOOL Pool,
                                                  IDirect3DVolumeTexture8 **ppVolumeTexture) = 0;
    virtual HRESULT __stdcall CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                                D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture) = 0;
    virtual HRESULT __stdcall CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                                 IDirect3DVertexBuffer8 **ppVertexBuffer) = 0;   // 0x5C
    virtual HRESULT __stdcall CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                IDirect3DIndexBuffer8 **ppIndexBuffer) = 0;
    virtual HRESULT __stdcall CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                                 D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable,
                                                 IDirect3DSurface8 **ppSurface) = 0;
    virtual HRESULT __stdcall CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                        D3DMULTISAMPLE_TYPE MultiSample,
                                                        IDirect3DSurface8 **ppSurface) = 0;
    virtual HRESULT __stdcall CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                 IDirect3DSurface8 **ppSurface) = 0;   // 0x6C
    virtual HRESULT __stdcall CopyRects(IDirect3DSurface8 *pSourceSurface, const RECT *pSourceRectsArray, UINT cRects,
                                        IDirect3DSurface8 *pDestinationSurface, const POINT *pDestPointsArray) = 0;
    virtual HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture,
                                            IDirect3DBaseTexture8 *pDestinationTexture) = 0;
    virtual HRESULT __stdcall GetFrontBuffer(IDirect3DSurface8 *pDestSurface) = 0;
    virtual HRESULT __stdcall SetRenderTarget(IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil) = 0;
    virtual HRESULT __stdcall GetRenderTarget(IDirect3DSurface8 **ppRenderTarget) = 0;
    virtual HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface) = 0;
    virtual HRESULT __stdcall BeginScene() = 0;                                             // 0x88
    virtual HRESULT __stdcall EndScene() = 0;
    virtual HRESULT __stdcall Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z,
                                    DWORD Stencil) = 0;                                     // 0x90
    virtual HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) = 0;
    virtual HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) = 0;
    virtual HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) = 0;
    virtual HRESULT __stdcall SetViewport(const D3DVIEWPORT8 *pViewport) = 0;               // 0xA0
    virtual HRESULT __stdcall GetViewport(D3DVIEWPORT8 *pViewport) = 0;
    virtual HRESULT __stdcall SetMaterial(const D3DMATERIAL8 *pMaterial) = 0;
    virtual HRESULT __stdcall GetMaterial(D3DMATERIAL8 *pMaterial) = 0;
    virtual HRESULT __stdcall SetLight(DWORD Index, const D3DLIGHT8 *pLight) = 0;           // 0xB0
    virtual HRESULT __stdcall GetLight(DWORD Index, D3DLIGHT8 *pLight) = 0;
    virtual HRESULT __stdcall LightEnable(DWORD Index, BOOL Enable) = 0;
    virtual HRESULT __stdcall GetLightEnable(DWORD Index, BOOL *pEnable) = 0;
    virtual HRESULT __stdcall SetClipPlane(DWORD Index, const float *pPlane) = 0;
    virtual HRESULT __stdcall GetClipPlane(DWORD Index, float *pPlane) = 0;
    virtual HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) = 0;    // 0xC8
    virtual HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) = 0;
    virtual HRESULT __stdcall BeginStateBlock() = 0;
    virtual HRESULT __stdcall EndStateBlock(DWORD *pToken) = 0;
    virtual HRESULT __stdcall ApplyStateBlock(DWORD Token) = 0;
    virtual HRESULT __stdcall CaptureStateBlock(DWORD Token) = 0;
    virtual HRESULT __stdcall DeleteStateBlock(DWORD Token) = 0;
    virtual HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD *pToken) = 0;
    virtual HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS8 *pClipStatus) = 0;
    virtual HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS8 *pClipStatus) = 0;
    virtual HRESULT __stdcall GetTexture(DWORD Stage, IDirect3DBaseTexture8 **ppTexture) = 0;
    virtual HRESULT __stdcall SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture) = 0;   // 0xF4
    virtual HRESULT __stdcall GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) = 0;
    virtual HRESULT __stdcall SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) = 0;   // 0xFC
    virtual HRESULT __stdcall ValidateDevice(DWORD *pNumPasses) = 0;
    virtual HRESULT __stdcall GetInfo(DWORD DevInfoID, void *pDevInfoStruct, DWORD DevInfoStructSize) = 0;
    virtual HRESULT __stdcall SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) = 0;
    virtual HRESULT __stdcall GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) = 0;
    virtual HRESULT __stdcall SetCurrentTexturePalette(UINT PaletteNumber) = 0;
    virtual HRESULT __stdcall GetCurrentTexturePalette(UINT *PaletteNumber) = 0;
    virtual HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) = 0;   // 0x118
    virtual HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT minIndex, UINT NumVertices,
                                                   UINT startIndex, UINT primCount) = 0;
    virtual HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                              const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) = 0;   // 0x120
    virtual HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                                     UINT NumVertexIndices, UINT PrimitiveCount, const void *pIndexData,
                                                     D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData,
                                                     UINT VertexStreamZeroStride) = 0;   // 0x124
    virtual HRESULT __stdcall ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                                              IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags) = 0;
    virtual HRESULT __stdcall CreateVertexShader(const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle,
                                                 DWORD Usage) = 0;
    virtual HRESULT __stdcall SetVertexShader(DWORD Handle) = 0;                            // 0x130
    virtual HRESULT __stdcall GetVertexShader(DWORD *pHandle) = 0;
    virtual HRESULT __stdcall DeleteVertexShader(DWORD Handle) = 0;
    virtual HRESULT __stdcall SetVertexShaderConstant(DWORD Register, const void *pConstantData,
                                                      DWORD ConstantCount) = 0;            // 0x13C
    virtual HRESULT __stdcall GetVertexShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) = 0;
    virtual HRESULT __stdcall GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData) = 0;
    virtual HRESULT __stdcall GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) = 0;
    virtual HRESULT __stdcall SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride) = 0;   // 0x14C
    virtual HRESULT __stdcall GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData,
                                              UINT *pStride) = 0;
    virtual HRESULT __stdcall SetIndices(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex) = 0;   // 0x154
    virtual HRESULT __stdcall GetIndices(IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex) = 0;
    virtual HRESULT __stdcall CreatePixelShader(const DWORD *pFunction, DWORD *pHandle) = 0;
    virtual HRESULT __stdcall SetPixelShader(DWORD Handle) = 0;                             // 0x160
    virtual HRESULT __stdcall GetPixelShader(DWORD *pHandle) = 0;
    virtual HRESULT __stdcall DeletePixelShader(DWORD Handle) = 0;
    virtual HRESULT __stdcall SetPixelShaderConstant(DWORD Register, const void *pConstantData,
                                                     DWORD ConstantCount) = 0;             // 0x16C
    virtual HRESULT __stdcall GetPixelShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount) = 0;
    virtual HRESULT __stdcall GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData) = 0;
    virtual HRESULT __stdcall DrawRectPatch(UINT Handle, const float *pNumSegs,
                                            const D3DRECTPATCH_INFO *pRectPatchInfo) = 0;
    virtual HRESULT __stdcall DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) = 0;
    virtual HRESULT __stdcall DeletePatch(UINT Handle) = 0;
};

struct IDirect3DSwapChain8 : public IUnknown {
    virtual HRESULT __stdcall Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride,
                                      const RGNDATA *pDirtyRegion) = 0;
    virtual HRESULT __stdcall GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type,
                                            IDirect3DSurface8 **ppBackBuffer) = 0;
};

struct IDirect3DResource8 : public IUnknown {
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID refguid) = 0;
    virtual DWORD __stdcall SetPriority(DWORD PriorityNew) = 0;
    virtual DWORD __stdcall GetPriority() = 0;
    virtual void __stdcall PreLoad() = 0;
    virtual D3DRESOURCETYPE __stdcall GetType() = 0;
};

struct IDirect3DBaseTexture8 : public IDirect3DResource8 {
    virtual DWORD __stdcall SetLOD(DWORD LODNew) = 0;
    virtual DWORD __stdcall GetLOD() = 0;
    virtual DWORD __stdcall GetLevelCount() = 0;
};

struct IDirect3DTexture8 : public IDirect3DBaseTexture8 {
    virtual HRESULT __stdcall GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) = 0;
    virtual HRESULT __stdcall GetSurfaceLevel(UINT Level, IDirect3DSurface8 **ppSurfaceLevel) = 0;
    virtual HRESULT __stdcall LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockRect(UINT Level) = 0;
    virtual HRESULT __stdcall AddDirtyRect(const RECT *pDirtyRect) = 0;
};

struct IDirect3DVolumeTexture8 : public IDirect3DBaseTexture8 {
    virtual HRESULT __stdcall GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) = 0;
    virtual HRESULT __stdcall GetVolumeLevel(UINT Level, IDirect3DVolume8 **ppVolumeLevel) = 0;
    virtual HRESULT __stdcall LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockBox(UINT Level) = 0;
    virtual HRESULT __stdcall AddDirtyBox(const D3DBOX *pDirtyBox) = 0;
};

struct IDirect3DCubeTexture8 : public IDirect3DBaseTexture8 {
    virtual HRESULT __stdcall GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) = 0;
    virtual HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level,
                                                IDirect3DSurface8 **ppCubeMapSurface) = 0;
    virtual HRESULT __stdcall LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect,
                                       const RECT *pRect, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) = 0;
    virtual HRESULT __stdcall AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) = 0;
};

struct IDirect3DVertexBuffer8 : public IDirect3DResource8 {
    virtual HRESULT __stdcall Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC *pDesc) = 0;
};

struct IDirect3DIndexBuffer8 : public IDirect3DResource8 {
    virtual HRESULT __stdcall Lock(UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC *pDesc) = 0;
};

struct IDirect3DSurface8 : public IUnknown {
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID refguid) = 0;
    virtual HRESULT __stdcall GetContainer(REFIID riid, void **ppContainer) = 0;
    virtual HRESULT __stdcall GetDesc(D3DSURFACE_DESC *pDesc) = 0;
    virtual HRESULT __stdcall LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockRect() = 0;
};

struct IDirect3DVolume8 : public IUnknown {
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice8 **ppDevice) = 0;
    virtual HRESULT __stdcall SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) = 0;
    virtual HRESULT __stdcall GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) = 0;
    virtual HRESULT __stdcall FreePrivateData(REFGUID refguid) = 0;
    virtual HRESULT __stdcall GetContainer(REFIID riid, void **ppContainer) = 0;
    virtual HRESULT __stdcall GetDesc(D3DVOLUME_DESC *pDesc) = 0;
    virtual HRESULT __stdcall LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) = 0;
    virtual HRESULT __stdcall UnlockBox() = 0;
};

#define D3DERR_WRONGTEXTUREFORMAT MAKE_HRESULT(1, 0x876, 2072)
#define D3DERR_UNSUPPORTEDCOLOROPERATION MAKE_HRESULT(1, 0x876, 2073)
#define D3DERR_UNSUPPORTEDCOLORARG MAKE_HRESULT(1, 0x876, 2074)
#define D3DERR_UNSUPPORTEDALPHAOPERATION MAKE_HRESULT(1, 0x876, 2075)
#define D3DERR_UNSUPPORTEDALPHAARG MAKE_HRESULT(1, 0x876, 2076)
#define D3DERR_TOOMANYOPERATIONS MAKE_HRESULT(1, 0x876, 2077)
#define D3DERR_CONFLICTINGTEXTUREFILTER MAKE_HRESULT(1, 0x876, 2078)
#define D3DERR_UNSUPPORTEDFACTORVALUE MAKE_HRESULT(1, 0x876, 2079)
#define D3DERR_CONFLICTINGRENDERSTATE MAKE_HRESULT(1, 0x876, 2081)
#define D3DERR_UNSUPPORTEDTEXTUREFILTER MAKE_HRESULT(1, 0x876, 2082)
#define D3DERR_CONFLICTINGTEXTUREPALETTE MAKE_HRESULT(1, 0x876, 2086)
#define D3DERR_DRIVERINTERNALERROR MAKE_HRESULT(1, 0x876, 2087)
#define D3DERR_NOTFOUND MAKE_HRESULT(1, 0x876, 2150)
#define D3DERR_MOREDATA MAKE_HRESULT(1, 0x876, 2151)
#define D3DERR_DEVICELOST MAKE_HRESULT(1, 0x876, 2152)
#define D3DERR_DEVICENOTRESET MAKE_HRESULT(1, 0x876, 2153)
#define D3DERR_NOTAVAILABLE MAKE_HRESULT(1, 0x876, 2154)
#define D3DERR_OUTOFVIDEOMEMORY MAKE_HRESULT(1, 0x876, 380)
#define D3DERR_INVALIDDEVICE MAKE_HRESULT(1, 0x876, 2155)
#define D3DERR_INVALIDCALL MAKE_HRESULT(1, 0x876, 2156)
#define D3DERR_DRIVERINVALIDCALL MAKE_HRESULT(1, 0x876, 2157)

extern "C" IDirect3D8 *__stdcall Direct3DCreate8(UINT SDKVersion);
