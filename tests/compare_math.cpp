// Differential test of the exported math functions: the original LS3DF.dll (loaded from the game
// folder) against the port (src/math/math.cpp, linked in). Every function gets the same random and
// edge-case inputs; results are compared by ULP distance.
//
// Usage: compare_math.exe [original LS3DF.dll] [rounds] [--pc24]
//   --pc24  run with the FPU in single precision, as the game does once Direct3D is up.
#include <windows.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>

#include "../src/math/math.cpp"
#include "test_paths.h"

namespace {

HMODULE g_orig;

template <class F> F Get(const char *name)
{
    F f = reinterpret_cast<F>(GetProcAddress(g_orig, name));
    if (!f) {
        std::printf("missing export %s\n", name);
        std::exit(2);
    }
    return f;
}

int64_t Ulps(float a, float b)
{
    if (std::isnan(a) && std::isnan(b))
        return 0;
    if (std::isnan(a) || std::isnan(b))
        return INT32_MAX;
    int32_t ia, ib;
    std::memcpy(&ia, &a, 4);
    std::memcpy(&ib, &b, 4);
    if (ia < 0) ia = INT32_MIN - ia;
    if (ib < 0) ib = INT32_MIN - ib;
    return std::llabs(int64_t(ia) - int64_t(ib));
}

struct Stat {
    long calls = 0, exact = 0, off = 0;
    int64_t maxUlps = 0;
    std::string worst;
};
std::map<std::string, Stat> g_stats;

void Check(const char *fn, const float *a, const float *b, int n, const char *what = "")
{
    Stat &s = g_stats[fn];
    ++s.calls;
    int64_t worst = 0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, Ulps(a[i], b[i]));
    if (worst == 0) {
        ++s.exact;
        return;
    }
    ++s.off;
    if (worst > s.maxUlps) {
        s.maxUlps = worst;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s orig[0..2]=%.9g %.9g %.9g port=%.9g %.9g %.9g", what, a[0],
                      n > 1 ? a[1] : 0, n > 2 ? a[2] : 0, b[0], n > 1 ? b[1] : 0, n > 2 ? b[2] : 0);
        s.worst = buf;
    }
}
void CheckBool(const char *fn, bool a, bool b)
{
    float fa = a, fb = b;
    Check(fn, &fa, &fb, 1);
}

std::mt19937 g_rng(12345);

float Rnd(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

// Mostly ordinary values; sometimes exact zeros, ones, tiny and huge values.
float Val()
{
    switch (g_rng() % 16) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return -1.0f;
    case 3: return (g_rng() % 2 ? 1 : -1) * Rnd(1e-5f, 1e-3f);   // small, but no exponent-range effects
    case 4: return Rnd(-1000.0f, 1000.0f);
    default: return Rnd(-2.0f, 2.0f);
    }
}
S_vector Vec()
{
    S_vector v(Val(), Val(), Val());
    if (g_rng() % 6 == 0) {                   // unit
        float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (l > 0) v = S_vector(v.x / l, v.y / l, v.z / l);
    }
    return v;
}
S_quat Quat()
{
    S_quat q;
    q.v = S_vector(Rnd(-1, 1), Rnd(-1, 1), Rnd(-1, 1));
    q.s = Rnd(-1, 1);
    if (g_rng() % 3) {
        float l = std::sqrt(q.v.x * q.v.x + q.v.y * q.v.y + q.v.z * q.v.z + q.s * q.s);
        q.v = S_vector(q.v.x / l, q.v.y / l, q.v.z / l);
        q.s /= l;
    }
    if (g_rng() % 10 == 0) q.Identity();
    return q;
}
S_matrix Mat()
{
    S_matrix m;
    switch (g_rng() % 6) {
    case 0: {                                  // rotation * scale + translation
        S_quat q = Quat();
        q.Normalize();
        m = q.RotationMatrix();
        float sx = Rnd(0.1f, 3), sy = g_rng() % 2 ? sx : Rnd(0.1f, 3), sz = g_rng() % 2 ? sx : Rnd(0.1f, 3);
        for (int c = 0; c < 3; ++c) { m.m[0][c] *= sx; m.m[1][c] *= sy; m.m[2][c] *= sz; }
        m(3) = Vec();
        break;
    }
    case 1: {                                  // degenerate: an axis zeroed
        m = Mat();
        int r = g_rng() % 3;
        m.m[r][0] = m.m[r][1] = m.m[r][2] = 0;
        break;
    }
    case 2:
        m = kIdentityMatrix;
        break;
    default:
        for (auto &row : m.m)
            for (float &f : row) f = Val();
    }
    return m;
}

void TestAll(int rounds)
{
    using MatOp = S_matrix *(__stdcall *)(const S_matrix *, S_matrix *, const S_matrix *);
    using MatMulEq = S_matrix *(__stdcall *)(S_matrix *, const S_matrix *);
    using Mat3Op = S_matrix3 *(__stdcall *)(const S_matrix3 *, S_matrix3 *, const S_matrix3 *);
    using Mat3MulEq = S_matrix3 *(__stdcall *)(S_matrix3 *, const S_matrix3 *);
    using VecMat = S_vector *(__stdcall *)(const S_vector *, S_vector *, const void *);
    using VecMatEq = S_vector *(__stdcall *)(S_vector *, const void *);
    using RotM = S_matrix *(__stdcall *)(S_matrix *, float);
    using MatVoid = void(__stdcall *)(S_matrix *);
    using MatInv = bool(__stdcall *)(S_matrix *, const S_matrix *);
    using Mat3Inv = bool(__stdcall *)(S_matrix3 *, const S_matrix3 *);
    using MatDir = void(__stdcall *)(S_matrix *, const S_vector *);
    using MatDirF = void(__stdcall *)(S_matrix *, const S_vector *, float);
    using MatDirV = void(__stdcall *)(S_matrix *, const S_vector *, const S_vector *);
    using MatQuat = void(__stdcall *)(S_matrix *, const S_quat *);
    using MatSetRotM = void(__stdcall *)(S_matrix *, const S_matrix *);
    using MatScale = S_vector *(__stdcall *)(const S_matrix *, S_vector *);
    using MatUScale = float(__stdcall *)(const S_matrix *);
    using VecAngle = float(__stdcall *)(const S_vector *, const S_vector *);
    using Vec2Angle = float(__stdcall *)(const S_vector2 *, const S_vector2 *);
    using QuatMat = void(__stdcall *)(S_quat *, const S_matrix *);
    using QuatAxis = void(__stdcall *)(S_quat *, const S_vector *, float);
    using QuatRv = void(__stdcall *)(S_quat *, const S_vector *);
    using QuatNorm = void(__stdcall *)(S_quat *);
    using QuatDir = S_vector *(__stdcall *)(const S_quat *, S_vector *);
    using QuatInv = void(__stdcall *)(const S_quat *, S_vector *, float *);
    using QuatRotM = S_matrix *(__stdcall *)(const S_quat *, S_matrix *);
    using QuatByM = S_quat *(__stdcall *)(const S_quat *, S_quat *, const S_matrix *);
    using QuatSlerp = S_quat *(__stdcall *)(const S_quat *, S_quat *, const S_quat *, float, bool);
    using Mat4Mul = S_matrix4 *(__stdcall *)(const S_matrix4 *, S_matrix4 *, const S_matrix4 *);
    using PlaneLine = bool(__stdcall *)(const S_plane *, const S_plane *, S_vector *, S_vector *);
    using LineLine = bool(__stdcall *)(const S_vector *, const S_vector *, const S_vector *, const S_vector *,
                                       float *, float *);
    using Rand = float (*)();

    auto mMul = Get<MatOp>("??DS_matrix@@QBG?AU0@ABU0@@Z");
    auto mMulEq = Get<MatMulEq>("??XS_matrix@@QAGAAU0@ABU0@@Z");
    auto m3Mul = Get<Mat3Op>("??DS_matrix3@@QBG?AU0@ABU0@@Z");
    auto m3MulEq = Get<Mat3MulEq>("??XS_matrix3@@QAGAAU0@ABU0@@Z");
    auto vMulM = Get<VecMat>("??DS_vector@@QBG?AU0@ABUS_matrix@@@Z");
    auto vMulM3 = Get<VecMat>("??DS_vector@@QBG?AU0@ABUS_matrix3@@@Z");
    auto vMulEqM = Get<VecMatEq>("??XS_vector@@QAGAAU0@ABUS_matrix@@@Z");
    auto vMulEqM3 = Get<VecMatEq>("??XS_vector@@QAGAAU0@ABUS_matrix3@@@Z");
    auto rotX = Get<RotM>("?RotateXMatrix@@YG?AUS_matrix@@M@Z");
    auto rotY = Get<RotM>("?RotateYMatrix@@YG?AUS_matrix@@M@Z");
    auto rotZ = Get<RotM>("?RotateZMatrix@@YG?AUS_matrix@@M@Z");
    auto mIdent = Get<MatVoid>("?Identity@S_matrix@@QAGXXZ");
    auto mInv = Get<MatInv>("?Inverse@S_matrix@@QAG_NABU1@@Z");
    auto m3Inv = Get<Mat3Inv>("?Inverse@S_matrix3@@QAG_NABU1@@Z");
    auto mSetDir = Get<MatDir>("?SetDir@S_matrix@@QAGXABUS_vector@@@Z");
    auto mSetDirF = Get<MatDirF>("?SetDir@S_matrix@@QAGXABUS_vector@@M@Z");
    auto mSetDirV = Get<MatDirV>("?SetDir@S_matrix@@QAGXABUS_vector@@0@Z");
    auto mSetDir3 = Get<MatDir>("?SetDir3@S_matrix@@QAGXABUS_vector@@@Z");
    auto mSetDir3F = Get<MatDirF>("?SetDir3@S_matrix@@QAGXABUS_vector@@M@Z");
    auto mSetDir3V = Get<MatDirV>("?SetDir3@S_matrix@@QAGXABUS_vector@@0@Z");
    auto mSetRotQ = Get<MatQuat>("?SetRot@S_matrix@@QAGXABUS_quat@@@Z");
    auto mSetRot3 = Get<MatQuat>("?SetRot3@S_matrix@@QAGXABUS_quat@@@Z");
    auto mSetInvRot = Get<MatQuat>("?SetInvRot@S_matrix@@QAGXABUS_quat@@@Z");
    auto mSetInvRot3 = Get<MatQuat>("?SetInvRot3@S_matrix@@QAGXABUS_quat@@@Z");
    auto mSetRotM = Get<MatSetRotM>("?SetRot@S_matrix@@QAGXABU1@@Z");
    auto mRotBy = Get<MatOp>("?RotateByMatrix@S_matrix@@QBG?AU1@ABU1@@Z");
    auto mScale = Get<MatScale>("?GetScale@S_matrix@@QBG?AUS_vector@@XZ");
    auto mScale2 = Get<MatScale>("?GetScale2@S_matrix@@QBG?AUS_vector@@XZ");
    auto mUScale = Get<MatUScale>("?GetUScale@S_matrix@@QBGMXZ");
    auto vAngle = Get<VecAngle>("?AngleTo@S_vector@@QBGMABU1@@Z");
    auto vCosAngle = Get<VecAngle>("?CosAngleTo@S_vector@@QBGMABU1@@Z");
    auto v2Angle = Get<Vec2Angle>("?AngleTo@S_vector2@@QBGMABU1@@Z");
    auto vRotBy = Get<VecMat>("?RotateByMatrix@S_vector@@QBG?AU1@ABUS_matrix@@@Z");
    auto vRotByN = Get<VecMat>("?RotateByNormMatrix@S_vector@@QBG?AU1@ABUS_matrix@@@Z");
    auto qMakeM = Get<QuatMat>("?Make@S_quat@@QAGXABUS_matrix@@@Z");
    auto qMakeA = Get<QuatAxis>("?Make@S_quat@@QAGXABUS_vector@@M@Z");
    auto qMakeV = Get<QuatRv>("?Make@S_quat@@QAGXABUS_vector@@@Z");
    auto qNorm = Get<QuatNorm>("?Normalize@S_quat@@QAGXXZ");
    auto qSetDir = Get<QuatAxis>("?SetDir@S_quat@@QAGXABUS_vector@@M@Z");
    auto qGetDir = Get<QuatDir>("?GetDir@S_quat@@QBG?AUS_vector@@XZ");
    auto qInv = Get<QuatInv>("?Inverse@S_quat@@QBGXAAUS_vector@@AAM@Z");
    auto qRotM = Get<QuatRotM>("?RotationMatrix@S_quat@@QBG?AUS_matrix@@XZ");
    auto qRotBy = Get<QuatByM>("?RotateByMatrix@S_quat@@QBG?AU1@ABUS_matrix@@@Z");
    auto qSlerp = Get<QuatSlerp>("?Slerp@S_quat@@QBG?AU1@ABU1@M_N@Z");
    auto m4Mul = Get<Mat4Mul>("?Mult4X4@S_matrix4@@QBG?AU1@ABU1@@Z");
    auto plLine = Get<PlaneLine>("?IntersectionLine@S_plane@@QBG_NABU1@AAUS_vector@@1@Z");
    auto llInt = Get<LineLine>("?LineLineIntersect@@YG_NABUS_vector@@000AAM1@Z");
    auto randF = Get<Rand>("RandFloat");

    for (int round = 0; round < rounds; ++round) {
        S_matrix a = Mat(), b = Mat(), ro, rp;
        mMul(&a, &ro, &b);
        rp = a * b;
        Check("S_matrix::operator*", &ro.m[0][0], &rp.m[0][0], 16);
        ro = a;
        mMulEq(&ro, &b);
        rp = a;
        rp *= b;
        Check("S_matrix::operator*=", &ro.m[0][0], &rp.m[0][0], 16);
        mRotBy(&a, &ro, &b);
        rp = a.RotateByMatrix(b);
        Check("S_matrix::RotateByMatrix", &ro.m[0][0], &rp.m[0][0], 16);

        S_matrix3 a3, b3, r3o, r3p;
        std::memcpy(&a3, &a, sizeof a3);
        std::memcpy(&b3, &b, sizeof b3);
        m3Mul(&a3, &r3o, &b3);
        r3p = a3 * b3;
        Check("S_matrix3::operator*", &r3o.m[0][0], &r3p.m[0][0], 12);
        r3o = a3;
        m3MulEq(&r3o, &b3);
        r3p = a3;
        r3p *= b3;
        Check("S_matrix3::operator*=", &r3o.m[0][0], &r3p.m[0][0], 12);
        r3o = b3;                              // (garbage in: the result is overwritten)
        bool bo = m3Inv(&r3o, &a3);
        r3p = b3;
        bool bp = r3p.Inverse(a3);
        CheckBool("S_matrix3::Inverse(bool)", bo, bp);
        Check("S_matrix3::Inverse", &r3o.m[0][0], &r3p.m[0][0], 12);

        ro = b;
        bo = mInv(&ro, &a);
        rp = b;
        bp = rp.Inverse(a);
        CheckBool("S_matrix::Inverse(bool)", bo, bp);
        Check("S_matrix::Inverse", &ro.m[0][0], &rp.m[0][0], 16);
        mIdent(&ro);
        rp.Identity();
        Check("S_matrix::Identity", &ro.m[0][0], &rp.m[0][0], 16);

        S_vector v = Vec(), w = Vec(), vo, vp;
        vMulM(&v, &vo, &a);
        vp = v * a;
        Check("S_vector::operator*(S_matrix)", &vo.x, &vp.x, 3);
        vMulM3(&v, &vo, &a3);
        vp = v * a3;
        Check("S_vector::operator*(S_matrix3)", &vo.x, &vp.x, 3);
        vo = v;
        vMulEqM(&vo, &a);
        vp = v;
        vp *= a;
        Check("S_vector::operator*=(S_matrix)", &vo.x, &vp.x, 3);
        vo = v;
        vMulEqM3(&vo, &a3);
        vp = v;
        vp *= a3;
        Check("S_vector::operator*=(S_matrix3)", &vo.x, &vp.x, 3);
        vRotBy(&v, &vo, &a);
        vp = v.RotateByMatrix(a);
        Check("S_vector::RotateByMatrix", &vo.x, &vp.x, 3);
        vRotByN(&v, &vo, &a);
        vp = v.RotateByNormMatrix(a);
        Check("S_vector::RotateByNormMatrix", &vo.x, &vp.x, 3);
        float fo = vAngle(&v, &w), fp = v.AngleTo(w);
        Check("S_vector::AngleTo", &fo, &fp, 1);
        fo = vCosAngle(&v, &w);
        fp = v.CosAngleTo(w);
        {
            char what[200];
            std::snprintf(what, sizeof what, "v=(%a %a %a) w=(%a %a %a)", v.x, v.y, v.z, w.x, w.y, w.z);
            Check("S_vector::CosAngleTo", &fo, &fp, 1, what);
        }
        S_vector2 v2 = {v.x, v.y}, w2 = {w.x, w.z};
        fo = v2Angle(&v2, &w2);
        fp = v2.AngleTo(w2);
        Check("S_vector2::AngleTo", &fo, &fp, 1);

        float ang = Rnd(-7, 7);
        if (g_rng() % 8 == 0) ang = Rnd(-1e-9f, 1e-9f);
        rotX(&ro, ang); rp = RotateXMatrix(ang);
        Check("RotateXMatrix", &ro.m[0][0], &rp.m[0][0], 16);
        rotY(&ro, ang); rp = RotateYMatrix(ang);
        Check("RotateYMatrix", &ro.m[0][0], &rp.m[0][0], 16);
        rotZ(&ro, ang); rp = RotateZMatrix(ang);
        Check("RotateZMatrix", &ro.m[0][0], &rp.m[0][0], 16);

        ro = rp = a;
        mSetDir(&ro, &v); rp.SetDir(v);
        Check("S_matrix::SetDir(v)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetDirF(&ro, &v, ang); rp.SetDir(v, ang);
        Check("S_matrix::SetDir(v,roll)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetDirV(&ro, &v, &w); rp.SetDir(v, w);
        Check("S_matrix::SetDir(v,up)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetDir3(&ro, &v); rp.SetDir3(v);
        Check("S_matrix::SetDir3(v)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetDir3F(&ro, &v, ang); rp.SetDir3(v, ang);
        Check("S_matrix::SetDir3(v,roll)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetDir3V(&ro, &v, &w); rp.SetDir3(v, w);
        Check("S_matrix::SetDir3(v,up)", &ro.m[0][0], &rp.m[0][0], 16);

        S_quat q = Quat(), q2 = Quat(), qo, qp;
        ro = rp = a;
        mSetRotQ(&ro, &q); rp.SetRot(q);
        Check("S_matrix::SetRot(q)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetRot3(&ro, &q); rp.SetRot3(q);
        Check("S_matrix::SetRot3(q)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetInvRot(&ro, &q); rp.SetInvRot(q);
        Check("S_matrix::SetInvRot(q)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = a;
        mSetInvRot3(&ro, &q); rp.SetInvRot3(q);
        Check("S_matrix::SetInvRot3(q)", &ro.m[0][0], &rp.m[0][0], 16);
        ro = rp = b;
        mSetRotM(&ro, &a); rp.SetRot(a);
        Check("S_matrix::SetRot(m)", &ro.m[0][0], &rp.m[0][0], 16);
        mScale(&a, &vo); vp = a.GetScale();
        Check("S_matrix::GetScale", &vo.x, &vp.x, 3);
        mScale2(&a, &vo); vp = a.GetScale2();
        Check("S_matrix::GetScale2", &vo.x, &vp.x, 3);
        fo = mUScale(&a); fp = a.GetUScale();
        Check("S_matrix::GetUScale", &fo, &fp, 1);

        qo = qp = q2;
        qMakeM(&qo, &a); qp.Make(a);
        Check("S_quat::Make(m)", &qo.v.x, &qp.v.x, 4);
        qo = qp = q2;
        qMakeA(&qo, &v, ang); qp.Make(v, ang);
        Check("S_quat::Make(axis,angle)", &qo.v.x, &qp.v.x, 4);
        qo = qp = q2;
        qMakeV(&qo, &v); qp.Make(v);
        Check("S_quat::Make(v)", &qo.v.x, &qp.v.x, 4);
        qo = qp = q;
        qNorm(&qo); qp.Normalize();
        Check("S_quat::Normalize", &qo.v.x, &qp.v.x, 4);
        qo = qp = q2;
        qSetDir(&qo, &v, ang); qp.SetDir(v, ang);
        Check("S_quat::SetDir", &qo.v.x, &qp.v.x, 4);
        qGetDir(&q, &vo); vp = q.GetDir();
        Check("S_quat::GetDir", &vo.x, &vp.x, 3);
        struct { S_vector axis; float guard; } io{}, ip{};
        float ao = 0, ap = 0;
        qInv(&q, &io.axis, &ao); q.Inverse(ip.axis, ap);
        Check("S_quat::Inverse(axis)", &io.axis.x, &ip.axis.x, 3);
        Check("S_quat::Inverse(angle)", &ao, &ap, 1);
        qRotM(&q, &ro); rp = q.RotationMatrix();
        Check("S_quat::RotationMatrix", &ro.m[0][0], &rp.m[0][0], 16);
        qRotBy(&q, &qo, &a); qp = q.RotateByMatrix(a);
        Check("S_quat::RotateByMatrix", &qo.v.x, &qp.v.x, 4);
        float t = Rnd(0, 1);
        qSlerp(&q, &qo, &q2, t, true); qp = q.Slerp(q2, t, true);
        Check("S_quat::Slerp", &qo.v.x, &qp.v.x, 4);

        S_matrix4 a4, b4, r4o, r4p;
        std::memcpy(&a4, &a, sizeof a4);
        std::memcpy(&b4, &b, sizeof b4);
        m4Mul(&a4, &r4o, &b4); r4p = a4.Mult4X4(b4);
        Check("S_matrix4::Mult4X4", &r4o.m[0][0], &r4p.m[0][0], 16);

        S_plane p1 = {Vec(), Val()}, p2 = {Vec(), Val()};
        if (g_rng() % 5 == 0) p2.normal = p1.normal;
        struct { S_vector v; float guard; } po{}, pp{}, dio{}, dip{};
        bo = plLine(&p1, &p2, &po.v, &dio.v);
        bp = p1.IntersectionLine(p2, pp.v, dip.v);
        CheckBool("S_plane::IntersectionLine(bool)", bo, bp);
        if (bo && bp) {
            Check("S_plane::IntersectionLine(pos)", &po.v.x, &pp.v.x, 3);
            Check("S_plane::IntersectionLine(dir)", &dio.v.x, &dip.v.x, 3);
        }
        S_vector p3 = Vec(), d3 = Vec(), p4 = Vec(), d4 = g_rng() % 6 ? Vec() : d3;
        float t1o = 0, t2o = 0, t1p = 0, t2p = 0;
        bo = llInt(&p3, &d3, &p4, &d4, &t1o, &t2o);
        bp = LineLineIntersect(p3, d3, p4, d4, t1p, t2p);
        CheckBool("LineLineIntersect(bool)", bo, bp);
        if (bo && bp) {
            float ro2[2] = {t1o, t2o}, rp2[2] = {t1p, t2p};
            Check("LineLineIntersect", ro2, rp2, 2);
        }
        (void)randF;
    }
    // RandFloat: both use their own CRT's rand() (seed 1), so the sequences match.
    for (int i = 0; i < 1000; ++i) {
        float fo = randF(), fp = RandFloat();
        Check("RandFloat", &fo, &fp, 1);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    const char *orig = OriginalDllPath(argc > 1 ? argv[1] : nullptr);
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 20000;
    bool pc24 = false;
    for (int i = 1; i < argc; ++i)
        pc24 |= std::strcmp(argv[i], "--pc24") == 0;
    char dir[MAX_PATH];
    std::strcpy(dir, orig);
    if (char *slash = std::strrchr(dir, '\\'))
        *slash = 0;
    SetDllDirectoryA(dir);
    g_orig = LoadLibraryA(orig);
    if (!g_orig) {
        std::printf("cannot load %s (error %lu)\n", orig, GetLastError());
        return 2;
    }
    if (pc24) {
        unsigned cw;
        _controlfp_s(&cw, _PC_24, _MCW_PC);
    }
    TestAll(rounds);
    long bad = 0;
    std::printf("%-36s %8s %8s %8s %8s\n", "function", "calls", "exact", "off", "maxulp");
    for (auto &[name, s] : g_stats) {
        std::printf("%-36s %8ld %8ld %8ld %8lld\n", name.c_str(), s.calls, s.exact, s.off, s.maxUlps);
        if (s.off)
            std::printf("    worst: %s\n", s.worst.c_str());
        bad += s.off;
    }
    std::printf("%s\n", bad ? "DIFFERENCES" : "all exact");
    return bad ? 1 : 0;
}
