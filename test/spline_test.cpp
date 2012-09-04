#include "stdafx.h"
#include "spline_test.hpp"
#include "../logger.hpp"
#include "../kumi_loader.hpp"
#include "../resource_interface.hpp"
#include "../scene.hpp"
#include "../threading.hpp"
#include "../mesh.hpp"
#include "../tweakable_param.hpp"
#include "../graphics.hpp"
#include "../material.hpp"
#include "../material_manager.hpp"
#include "../xmath.hpp"
#include "../profiler.hpp"
#include "../deferred_context.hpp"
#include "../animation_manager.hpp"
#include "../app.hpp"
#include "../bit_utils.hpp"
#include "../dx_utils.hpp"
#include "../vertex_types.hpp"

using namespace std;
using namespace std::tr1::placeholders;

#define SPLINE_DEBUG 0

static const int cNumParticles = 10000;

static const int cRingsPerSegment = 10;
static const int cSegmentHeight = 20;
static const int cRingSpacing = cSegmentHeight / cRingsPerSegment;
static const int cVertsPerRing = 20;
static const int cVertsPerSegment = cRingsPerSegment * cVertsPerRing;
static const int cMaxRingsPerBuffer = 1000;
static const float cChildRate = 2.0f;

// each entry in the data sent to the VS/GS is 2 frames and 2 radius
struct VsInput {
  XMFLOAT4X4 frame0, frame1;
  XMFLOAT2 radius;
};

XMFLOAT3 perpVector(const XMFLOAT3 &a) {
  // return a vector perpendicular to "a"
  float x = fabs(a.x), y = fabs(a.y), z = fabs(a.z);
  if (x <= y && x <= z) {
    return cross(a, XMFLOAT3(1,0,0));
  } else if (y <= x && y <= z) {
    return cross(a, XMFLOAT3(0,1,0));
  } else {
    return cross(a, XMFLOAT3(0,0,1));
  }
}

float evalBSpline(float t, float p0, float p1, float p2, float p3) {
  float t2 = t*t;
  float t3 = t2*t;

  XMFLOAT4 p = XMFLOAT4(p0, p1, p2, p3);

  float a = dot(XMFLOAT4(-1,  3, -3,  1), p);
  float b = dot(XMFLOAT4( 3, -6,  3,  0), p);
  float c = dot(XMFLOAT4(-3,  0,  3,  0), p);
  float d = dot(XMFLOAT4( 1,  4,  1,  0), p);

  return dot(XMFLOAT4(t3, t2, t, 1), XMFLOAT4(a, b, c, d)) / 6;
}

XMFLOAT3 evalBSpline(float t, const XMFLOAT3 &p0, const XMFLOAT3 &p1, const XMFLOAT3 &p2, const XMFLOAT3 &p3) {
  return XMFLOAT3(
    evalBSpline(t, p0.x, p1.x, p2.x, p3.x),
    evalBSpline(t, p0.y, p1.y, p2.y, p3.y),
    evalBSpline(t, p0.z, p1.z, p2.z, p3.z));
}

void stepBSpline(int pointsPerSegment, const vector<XMFLOAT3> &controlPoints, vector<XMFLOAT3> *out) {

  int numPts = (int)controlPoints.size();
  out->resize(pointsPerSegment * (numPts - 1));
  XMFLOAT3 *dst = out->data();

  for (int i = 0; i < numPts - 1; ++i) {
    auto &p0 = controlPoints[max(0, i-1)];
    auto &p1 = controlPoints[max(0, i-0)];
    auto &p2 = controlPoints[min(numPts-1, i+1)];
    auto &p3 = controlPoints[min(numPts-1, i+2)];

    for (int j = 0; j < pointsPerSegment; ++j) {
      float t = j / (float)pointsPerSegment;
      dst->x = evalBSpline(t, p0.x, p1.x, p2.x, p3.x);
      dst->y = evalBSpline(t, p0.y, p1.y, p2.y, p3.y);
      dst->z = evalBSpline(t, p0.z, p1.z, p2.z, p3.z);
      dst++;
    }
  }
}

typedef std::function<void (const XMFLOAT3 &p, const XMFLOAT3 &d, const XMFLOAT3 &n, DynamicSpline *parent)> cbCreateSpline;

class DynamicSpline {
public:
  DynamicSpline(DeferredContext *ctx, const XMFLOAT3 &p, const XMFLOAT3 &dir, const XMFLOAT3 &n, 
    float growthRate, float maxRadius, float maxHeight, const cbCreateSpline &cbCreateSpline)
    : _curTop(p)
    , _curHeight(0)
    , _elapsedTime(0)
    , _growthRate(growthRate)
    , _ctx(ctx)
    , _ringOffset(0)
    , _curControlPt(0)
    , _startRing(0)
    , _prevStartRing(0)
    , _dynamicRings(128)
    , _totalStaticSegments(0)
    , _maxRadius(maxRadius)
    , _maxHeight(maxHeight)
    , _cbCreateSpline(cbCreateSpline)
    , _nextChild(gaussianRand(cChildRate, 1))
  {
    _controlPoints.push_back(_curTop);

    for (int i = 0; i < 3; ++i) {
#if SPLINE_DEBUG
      _curTop.x += 0; //randf(-10, 10);
      _curTop.y += 10; //gaussianRand(10, 2);
      _curTop.z += 0; //randf(-10, 10);
#else
      _curTop.x += randf(-10.f, 10.f);
      _curTop.y += gaussianRand(10, 2);
      _curTop.z += randf(-10.f, 10.f);
#endif
      _controlPoints.push_back(_curTop);
    }

    const int numPts = _controlPoints.size();

    // calc initial reference frame
    _prevB = cross(dir, n);

    _vtxCache.push_back(VtxCache(p, n, dir, _prevB));

    // calc initial vertex cache
    for (int i = 0; i < numPts-2; ++i) {
      updateVtxCache();
    }
  }

  void updateVtxCache() {

    const int numPts = _controlPoints.size();
    auto &p0 = _controlPoints[max(0, _curControlPt-1)];
    auto &p1 = _controlPoints[_curControlPt];
    auto &p2 = _controlPoints[_curControlPt+1];
    auto &p3 = _controlPoints[_curControlPt+2];

    float step = 1.0f / cRingsPerSegment;
    float t = 0;
    auto cur = evalBSpline(t, p0, p1, p2, p3);
    for (int i = 0; i < cRingsPerSegment; ++i) {
      t += step;
      auto next = evalBSpline(t, p0, p1, p2, p3);
      auto dir = normalize(next - cur);
      auto n = cross(_prevB, dir);
      auto b = cross(dir, n);
      _vtxCache.push_back(VtxCache(cur, n, dir, b));
      cur = next;
      _prevB = b;
    }

    ++_curControlPt;
  }


  bool init() {
    return true;
  }

  const XMFLOAT3 &pos() const {
    return _curPos;
  }

  void update(float delta) {

    // if the delta is too large, then i'm probably just debugging, so lets throw this step away
    if (delta > 0.5f)
      return;

    _curHeight += _growthRate * delta;
    if (_curHeight > _maxHeight)
      return;

    _elapsedTime += delta;

    int curSegment = (int)(_curHeight / cSegmentHeight);
    int curRing = (int)(_curHeight / cRingSpacing);

    while (curSegment > (int)_controlPoints.size() - 4) {
#if SPLINE_DEBUG
      _curTop.x += 0; //randf(-10, 10);
      _curTop.y += 10; //gaussianRand(10, 2);
      _curTop.z += 0; //randf(-10, 10);
#else
      _curTop.x += randf(-10.f, 10.f);
      _curTop.y += gaussianRand(10, 2);
      _curTop.z += randf(-10.f, 10.f);
#endif
      _controlPoints.push_back(_curTop);
      updateVtxCache();
    }

#if SPLINE_DEBUG
    const float spikeLength = 0.01f;
#else
    const float spikeLength = 10;
#endif
    const float spikeStart = max(0,_curHeight - spikeLength);
    const int startRing = (int)(spikeStart / cRingSpacing);

    const float minRadius = 0.01f;
    const float maxRadius = _maxRadius;
    const float step = cRingSpacing;

    // if we're starting in a new ring, move the old one to the static buffer
    _staticRings.clear();
    if (_prevStartRing < startRing) {
      for (int i = _prevStartRing; i <= startRing; ++i) {
        VtxCache *cur = &_vtxCache[i - _ringOffset];
        addRing(cur->p, cur->n, cur->dir, cur->b, maxRadius, true);
      }
      _prevStartRing = startRing;
    }

    // throw away any vtx-cache elements that we no longer care about
    while (_ringOffset < startRing) {
      _ringOffset++;
      _vtxCache.pop_front();
    }

    _startRing = startRing;
    _dynamicRings.clear();

    const float startPos = (float)(startRing * cRingSpacing);
    float remaining = _curHeight - startPos;

    // add the bottom ring
    VtxCache *cur = &_vtxCache[startRing-_ringOffset];

    // check if we should create a new spline here
    if (_elapsedTime > _nextChild) {
      float x = randf(-0.1f, 0.1f);
      float y = randf(-0.1f, 0.1f);
      float z = randf(-0.1f, 0.1f);
      XMFLOAT3 d = normalize(cur->n + XMFLOAT3(x, y, z));
      _cbCreateSpline(cur->p, d, cross(d, cur->dir), this);
      _nextChild += gaussianRand(cChildRate, 1);
    }

    float r = clamp(minRadius, maxRadius, lerp(minRadius, maxRadius, remaining / spikeLength));
    addRing(cur->p, cur->n, cur->dir, cur->b, r, false);

    // add a ring where the spike starts
    if (remaining > spikeLength) {
      float t = (spikeStart - startPos) / step;
      KASSERT(t >= 0 && t < 1);
      VtxCache *next = &_vtxCache[startRing+1-_ringOffset];
      addRing(lerp(cur->p, next->p, t), lerp(cur->n, next->n, t), lerp(cur->dir, next->dir, t), lerp(cur->b, next->b, t), r, false);
    }
    remaining -= step;

    for (int i = startRing + 1; i <= curRing; ++i) {
      cur = &_vtxCache[i-_ringOffset];
      r = lerp(minRadius, maxRadius, clamp(0.f, 1.f, remaining / spikeLength));
      addRing(cur->p, cur->n, cur->dir, cur->b, r, false);
      remaining -= step;
    }

    {
      // add the end cap by lerping between cur and next rings
      float t = (_curHeight - curRing * step) / step;
      cur = &_vtxCache[curRing-_ringOffset];
      auto *next = &_vtxCache[curRing+1-_ringOffset];
      r = minRadius;
      XMFLOAT3 p = lerp(cur->p, next->p, t);
      addRing(p, lerp(cur->n, next->n, t), lerp(cur->dir, next->dir, t), lerp(cur->b, next->b, t), r, false);
      _curPos = p;
    }
  }

  void render(VsInput *staticVb, VsInput *dynamicVb, int *newStaticSegments, int *newDynamicSegments) {

    // add new static parts
    if (_staticRings.size() > 0) {
      int numEntries = _staticRings.size()  - 1;
      for (int i = 0; i < numEntries; ++i) {
        auto &ring0 = _staticRings[i];
        auto &ring1 = _staticRings[i+1];
        staticVb[i].frame0 = ring0.frame;
        staticVb[i].frame1 = ring1.frame;
        staticVb[i].radius = XMFLOAT2(ring0.radius, ring1.radius);
      }
      *newStaticSegments += numEntries;
    }

    // add new dynamic parts
    if (_dynamicRings.size() > 0) {
      int numEntries = _dynamicRings.size() - 1;
      for (int i = 0; i < numEntries; ++i) {
        auto &ring0 = _dynamicRings[i];
        auto &ring1 = _dynamicRings[i+1];
        dynamicVb[i].frame0 = ring0.frame;
        dynamicVb[i].frame1 = ring1.frame;
        dynamicVb[i].radius = XMFLOAT2(ring0.radius, ring1.radius);
      }
      *newDynamicSegments += numEntries;
    }
  }

  float growthRate() const { return _growthRate; }
  float maxRadius() const { return _maxRadius; }
  float maxHeight() const { return _maxHeight; }

private:

  struct Ring {
    Ring() {}
    Ring(const XMFLOAT4X4 &frame, float radius) : frame(frame), radius(radius) {}
    XMFLOAT4X4 frame;
    float radius;
  };

  void addRing(const XMFLOAT3 &p, const XMFLOAT3 &x, const XMFLOAT3 &y, const XMFLOAT3 &z, float r, bool staticRing) {

    XMFLOAT4X4 mtx;
    // Create transform from reference frame to world
    // NB: we directly store the transpose, as this is what HLSL wants
    set_col(expand(x, 0), 0, &mtx);
    set_col(expand(y, 0), 1, &mtx);
    set_col(expand(z, 0), 2, &mtx);
    set_col(expand(p, 1), 3, &mtx);

    if (staticRing)
      _staticRings.push_back(Ring(mtx, r));
    else
      _dynamicRings.push_back(Ring(mtx, r));
  }

  struct VtxCache {
    VtxCache() {}
    VtxCache(const XMFLOAT3 &p, const XMFLOAT3 &n, const XMFLOAT3 &dir, const XMFLOAT3 &b) : p(p), n(n), dir(dir), b(b) {}
    XMFLOAT3 p;
    XMFLOAT3 n, b, dir;
  };

  deque<VtxCache> _vtxCache;
  int _ringOffset;

  vector<Ring> _staticRings;
  vector<Ring> _dynamicRings;

  XMFLOAT3 _prevB;    // bitangent from previous iteration
  XMFLOAT3 _curTop;
  vector<XMFLOAT3> _controlPoints;
  float _curHeight;
  float _elapsedTime;
  float _growthRate;
  int _curControlPt;
  int _startRing;
  int _prevStartRing;

  int _totalStaticSegments;
  float _maxRadius;
  float _maxHeight;
  float _nextChild;

  cbCreateSpline _cbCreateSpline;
  XMFLOAT3 _curPos;

  DeferredContext *_ctx;
};

static float particleFade[512] = {0.0643f,0.0643f,0.0643f,0.0643f,0.0643f,0.0644f,0.0644f,0.0644f,0.0644f,0.0644f,0.0644f,0.0645f,0.0645f,0.0645f,0.0645f,0.0645f,0.0645f,0.0645f,0.0645f,0.0645f,0.0645f,0.0646f,0.0646f,0.0646f,0.0646f,0.0646f,0.0646f,0.0647f,0.0647f,0.0647f,0.0648f,0.0648f,0.0648f,0.0649f,0.0649f,0.0650f,0.0650f,0.0651f,0.0652f,0.0653f,0.0653f,0.0654f,0.0655f,0.0656f,0.0658f,0.0659f,0.0660f,0.0661f,0.0663f,0.0665f,0.0666f,0.0668f,0.0670f,0.0672f,0.0674f,0.0676f,0.0678f,0.0680f,0.0681f,0.0683f,0.0684f,0.0686f,0.0687f,0.0689f,0.0691f,0.0692f,0.0694f,0.0696f,0.0697f,0.0699f,0.0701f,0.0702f,0.0704f,0.0706f,0.0708f,0.0710f,0.0712f,0.0714f,0.0716f,0.0718f,0.0720f,0.0722f,0.0724f,0.0726f,0.0728f,0.0730f,0.0732f,0.0734f,0.0736f,0.0738f,0.0741f,0.0743f,0.0745f,0.0747f,0.0750f,0.0752f,0.0755f,0.0757f,0.0759f,0.0762f,0.0764f,0.0767f,0.0769f,0.0772f,0.0774f,0.0777f,0.0779f,0.0782f,0.0785f,0.0787f,0.0790f,0.0793f,0.0796f,0.0798f,0.0801f,0.0804f,0.0807f,0.0810f,0.0813f,0.0815f,0.0818f,0.0821f,0.0824f,0.0827f,0.0830f,0.0833f,0.0836f,0.0840f,0.0843f,0.0846f,0.0849f,0.0852f,0.0855f,0.0859f,0.0862f,0.0865f,0.0868f,0.0872f,0.0875f,0.0879f,0.0882f,0.0885f,0.0889f,0.0892f,0.0896f,0.0900f,0.0904f,0.0909f,0.0913f,0.0917f,0.0921f,0.0925f,0.0929f,0.0933f,0.0938f,0.0942f,0.0946f,0.0950f,0.0955f,0.0959f,0.0963f,0.0968f,0.0972f,0.0976f,0.0981f,0.0985f,0.0990f,0.0994f,0.0999f,0.1003f,0.1008f,0.1012f,0.1017f,0.1022f,0.1027f,0.1031f,0.1036f,0.1041f,0.1046f,0.1051f,0.1056f,0.1061f,0.1066f,0.1071f,0.1076f,0.1082f,0.1087f,0.1092f,0.1098f,0.1103f,0.1108f,0.1114f,0.1120f,0.1125f,0.1131f,0.1137f,0.1143f,0.1148f,0.1154f,0.1160f,0.1167f,0.1173f,0.1179f,0.1185f,0.1191f,0.1198f,0.1204f,0.1211f,0.1218f,0.1224f,0.1231f,0.1238f,0.1245f,0.1252f,0.1259f,0.1266f,0.1273f,0.1281f,0.1288f,0.1295f,0.1301f,0.1307f,0.1312f,0.1318f,0.1322f,0.1327f,0.1331f,0.1335f,0.1338f,0.1342f,0.1345f,0.1347f,0.1350f,0.1353f,0.1355f,0.1357f,0.1359f,0.1361f,0.1363f,0.1365f,0.1367f,0.1368f,0.1370f,0.1372f,0.1374f,0.1376f,0.1378f,0.1380f,0.1382f,0.1384f,0.1387f,0.1389f,0.1392f,0.1395f,0.1398f,0.1402f,0.1406f,0.1410f,0.1414f,0.1419f,0.1424f,0.1430f,0.1436f,0.1442f,0.1449f,0.1456f,0.1464f,0.1472f,0.1481f,0.1490f,0.1500f,0.1510f,0.1521f,0.1533f,0.1545f,0.1558f,0.1571f,0.1585f,0.1600f,0.1616f,0.1632f,0.1649f,0.1667f,0.1686f,0.1706f,0.1726f,0.1747f,0.1769f,0.1793f,0.1817f,0.1842f,0.1867f,0.1894f,0.1922f,0.1951f,0.1981f,0.2019f,0.2070f,0.2124f,0.2180f,0.2240f,0.2302f,0.2366f,0.2433f,0.2502f,0.2573f,0.2646f,0.2722f,0.2799f,0.2878f,0.2959f,0.3041f,0.3125f,0.3210f,0.3296f,0.3384f,0.3473f,0.3562f,0.3653f,0.3744f,0.3837f,0.3929f,0.4022f,0.4116f,0.4210f,0.4304f,0.4398f,0.4493f,0.4587f,0.4681f,0.4774f,0.4868f,0.4960f,0.5052f,0.5144f,0.5234f,0.5324f,0.5413f,0.5501f,0.5587f,0.5672f,0.5756f,0.5838f,0.5919f,0.5998f,0.6075f,0.6153f,0.6250f,0.6347f,0.6445f,0.6544f,0.6642f,0.6741f,0.6841f,0.6940f,0.7040f,0.7139f,0.7238f,0.7336f,0.7435f,0.7532f,0.7629f,0.7726f,0.7821f,0.7916f,0.8010f,0.8102f,0.8193f,0.8283f,0.8371f,0.8458f,0.8543f,0.8627f,0.8708f,0.8788f,0.8866f,0.8941f,0.9014f,0.9085f,0.9154f,0.9220f,0.9283f,0.9343f,0.9401f,0.9455f,0.9507f,0.9560f,0.9609f,0.9654f,0.9696f,0.9734f,0.9770f,0.9802f,0.9832f,0.9858f,0.9883f,0.9905f,0.9924f,0.9942f,0.9957f,0.9971f,0.9983f,0.9993f,1.0002f,1.0009f,1.0016f,1.0021f,1.0026f,1.0029f,1.0032f,1.0035f,1.0037f,1.0040f,1.0042f,1.0044f,1.0046f,1.0049f,1.0052f,1.0056f,1.0061f,1.0067f,1.0074f,1.0081f,1.0089f,1.0097f,1.0105f,1.0113f,1.0121f,1.0129f,1.0136f,1.0144f,1.0151f,1.0157f,1.0163f,1.0169f,1.0174f,1.0178f,1.0181f,1.0183f,1.0184f,1.0185f,1.0184f,1.0182f,1.0178f,1.0173f,1.0167f,1.0159f,1.0150f,1.0139f,1.0126f,1.0111f,1.0095f,1.0076f,1.0056f,1.0033f,1.0008f,0.9971f,0.9927f,0.9881f,0.9832f,0.9780f,0.9725f,0.9667f,0.9606f,0.9541f,0.9473f,0.9402f,0.9327f,0.9248f,0.9165f,0.9078f,0.8987f,0.8892f,0.8793f,0.8688f,0.8580f,0.8466f,0.8348f,0.8225f,0.8041f,0.7836f,0.7617f,0.7384f,0.7141f,0.6888f,0.6628f,0.6362f,0.6092f,0.5820f,0.5547f,0.5276f,0.5008f,0.4745f,0.4489f,0.4241f,0.3953f,0.3652f,0.3341f,0.3026f,0.2709f,0.2395f,0.2088f,0.1792f,0.1511f,0.1249f,0.1010f,0.0798f,0.0617f,0.0500f,0.0500f,0.0500f,0.0500f};
/*[{"x":0.5382888381545609,"y":0.06428571428571428},{"x":10.990988241659629,"y":0.06785714285714284},{"x":28.018015268686657,"y":0.0892857142857143},{"x":42.597373971786595,"y":0.12857142857142856},{"x":57.84952649331181,"y":0.19999999999999996},{"x":67.68962489429582,"y":0.6142857142857143},{"x":75.31570115505842,"y":0.95},{"x":82.32677126575953,"y":1.0071428571428571},{"x":89.09909634976773,"y":1},{"x":93.5585558092272,"y":0.8214285714285714},{"x":96.6666639173353,"y":0.42500000000000004},{"x":99.36936662003801,"y":0.050000000000000044}]*/

/*
[{"x":3.728985287899754,"y":0.8500000000000001},{"x":41.367361671663595,"y":0.8714285714285714},{"x":65.35260152406211,"y":0.8571428571428572},{"x":76.05370853513223,"y":0.6964285714285712},{"x":89.21484014644834,"y":-0.05357142857142838}]
*/

struct ParticleVtx {
  XMFLOAT3 pos;
  XMFLOAT2 scale;
};


SplineTest::SplineTest(const std::string &name) 
  : Effect(name)
  , _ctx(nullptr)
  , _staticVertCount(0)
{
  ZeroMemory(_keystate, sizeof(_keystate));
}

SplineTest::~SplineTest() {
  seq_delete(&_splines);
  GRAPHICS.destroy_deferred_context(_ctx);
}

void SplineTest::createSplineCallback(const XMFLOAT3 &p, const XMFLOAT3 &dir, const XMFLOAT3 &n, DynamicSpline *parent) {

  auto cb = bind(&SplineTest::createSplineCallback, this, _1, _2, _3, _4);
#if SPLINE_DEBUG
  const int maxSplines = 0;
#else
  const int maxSplines = 1500;
#endif

  if (_splines.size() > maxSplines)
    return;

  unique_ptr<DynamicSpline> spline(new DynamicSpline(_ctx, p, dir, n, 
    parent->growthRate() * 0.75f, parent->maxRadius() * 0.75f, parent->maxHeight() * 0.1f,
    cb));
  if (!spline->init())
    return;
  _splines.push_back(spline.release());
}


bool SplineTest::init() {

  int w = GRAPHICS.width();
  int h = GRAPHICS.height();

  B_ERR_BOOL(GRAPHICS.load_techniques("effects/particles.tec", true));
  B_ERR_BOOL(GRAPHICS.load_techniques("effects/scale.tec", true));
  B_ERR_BOOL(GRAPHICS.load_techniques("effects/splines.tec", true));

  _particle_technique = GRAPHICS.find_technique("spline_particles");
  _gradient_technique = GRAPHICS.find_technique("spline_gradient");
  _compose_technique = GRAPHICS.find_technique("spline_compose");

  _splineTechnique = GRAPHICS.find_technique("spline_render");
  _planeTechnique = GRAPHICS.find_technique("spline_plane");

  _particle_texture = RESOURCE_MANAGER.load_texture("gfx/particle1.png", "particle1.png", false, nullptr);
  _cubeMap = RESOURCE_MANAGER.load_texture("gfx/pearly3_cubemap2.dds", "pearly3_cubemap.dds", false, nullptr);

  _scale = GRAPHICS.find_technique("scale");

  _ctx = GRAPHICS.create_deferred_context(true);

  if (!_blur.init()) {
    return false;
  }

  _freefly_camera.setPos(XMFLOAT3(0, 0, -50));

  if (!initSplines())
    return false;

  _staticVb = GFX_create_buffer(D3D11_BIND_VERTEX_BUFFER, 64 * 1024 * 1024, true, nullptr, sizeof(VsInput));
  _dynamicVb = GFX_create_buffer(D3D11_BIND_VERTEX_BUFFER, 4 * 1024 * 1024, true, nullptr, sizeof(VsInput));

  _particleVb = GFX_create_buffer(D3D11_BIND_VERTEX_BUFFER, 1024 * 1024, true, nullptr, sizeof(ParticleVtx));

  D3D11_MAPPED_SUBRESOURCE res;
  _ctx->map(_particleVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
  PosTex *particles = (PosTex *)res.pData;
  for (int i = 0; i < cNumParticles; ++i) {
    float particleRadius = randf<float>(1, 5000);
    float angle = randf<float>(0, 2 * XM_PI);
    float h = randf<float>(0, 500);
    particles[i].pos = XMFLOAT3(particleRadius * sinf(angle), h, particleRadius * cosf(angle));
    float s = gaussianRand(2, 1);
    particles[i].tex = XMFLOAT2(s, 1);
  }

  _ctx->unmap(_particleVb, 0);

  XMFLOAT3 planeVerts[] = {
    XMFLOAT3(-1000, 0, -1000),
    XMFLOAT3(-1000, 0, +1000),
    XMFLOAT3(+1000, 0, +1000),
    XMFLOAT3(+1000, 0, -1000),
  };

  int planeIndices[] = {
    0, 1, 2,
    0, 2, 3,
  };

  _planeVb = GFX_create_buffer(D3D11_BIND_VERTEX_BUFFER, sizeof(planeVerts), false, planeVerts, sizeof(XMFLOAT3));
  _planeIb = GFX_create_buffer(D3D11_BIND_INDEX_BUFFER, sizeof(planeIndices), false, planeIndices, DXGI_FORMAT_R32_UINT);


  return true;
}

bool SplineTest::initSplines() {

  auto cb = bind(&SplineTest::createSplineCallback, this, _1, _2, _3, _4);

#if SPLINE_DEBUG
  const int numSplines = 1;
  float span = 0;
#else
  float span = 0;
  const int numSplines = 1;
#endif
  for (int i = 0; i < numSplines; ++i) {
    float x = randf(-span, span);
    float z = randf(-span, span);
    float g = gaussianRand(15, 10);
    float r = 1.5f;
#if SPLINE_DEBUG
    g = 5;
    r = 10;
#endif
    DynamicSpline *spline = new DynamicSpline(_ctx, XMFLOAT3(x,0,z), XMFLOAT3(0,1,0), XMFLOAT3(1,0,0), g, r, 10000, cb);
    if (!spline || !spline->init())
      return false;
    _splines.push_back(spline);
  }

  float angle = 0;
  float step = 2*XM_PI/cVertsPerRing;
  // output an extra vertex to avoid a "%" in the shader
  for (int i = 0; i <= cVertsPerRing; ++i) {
    float x = sinf(angle);
    float y = 0;
    float z = cosf(angle);
    angle += step;
    _extrudeShape.push_back(XMFLOAT4(x,y,z, 1));
  }

  return true;
}

void SplineTest::calc_camera_matrices(double time, double delta, XMFLOAT4X4 *view, XMFLOAT4X4 *proj) {

  *proj = _freefly_camera.projectionMatrix();
  if (_useFreeflyCamera) {
    _cameraPos = _freefly_camera.pos();
    *view = _freefly_camera.viewMatrix();
  } else {

    float radius = 75;
    XMFLOAT3 p = _splines.front()->pos();
    XMVECTOR at =  XMLoadFloat4(&XMFLOAT4(p.x, p.y - 20, p.z, 0));
    float s = sinf((float)time/10);
    float c = -cosf((float)time/10);
    XMFLOAT3 pp = p + radius * XMFLOAT3(s, s*c, c);
    _cameraPos = pp;
    XMVECTOR pos = XMLoadFloat4(&XMFLOAT4(pp.x, pp.y + 2, pp.z, 0));
    XMVECTOR up = XMLoadFloat4(&XMFLOAT4(0, 1, 0, 0));
    XMMATRIX lookat = XMMatrixLookAtLH(pos, at, up);
    XMStoreFloat4x4(view, lookat);
  }
}

bool SplineTest::update(int64 global_time, int64 local_time, int64 delta_ns, bool paused, int64 frequency, int32 num_ticks, float ticks_fraction) {
  ADD_PROFILE_SCOPE();

  double time = local_time  / 1000.0;

  for (size_t i = 0; i < _splines.size(); ++i) {
    _splines[i]->update(paused ? 0 : (delta_ns / 1e6f));
  }

  calc_camera_matrices(time, delta_ns / 1e6, &_view, &_proj);

  return true;
}

void SplineTest::post_process(GraphicsObjectHandle input, GraphicsObjectHandle output, GraphicsObjectHandle technique) {
  if (output.is_valid())
    _ctx->set_render_target(output, true);
  else
    _ctx->set_default_render_target(false);

  TextureArray arr = { input };
  _ctx->render_technique(technique, bind(&SplineTest::fill_cbuffer, this, _1), arr, DeferredContext::InstanceData());

  if (output.is_valid())
    _ctx->unset_render_targets(0, 1);
}

void SplineTest::renderPlane(GraphicsObjectHandle rtMirror) {
  int w = GRAPHICS.width();
  int h = GRAPHICS.height();

  Technique *technique;
  Shader *vs, *gs, *ps;
  setupTechnique(_ctx, _planeTechnique, false, &technique, &vs, &gs, &ps);

  struct {
    XMFLOAT4X4 worldViewProj;
    XMFLOAT4X4 mirrorViewProj;
  } cbuffer;

  XMMATRIX xmViewProj = XMMatrixMultiply(XMLoadFloat4x4(&_view), XMLoadFloat4x4(&_proj));
  XMStoreFloat4x4(&cbuffer.worldViewProj, XMMatrixTranspose(xmViewProj));

  XMMATRIX reflect = XMMatrixReflect(XMLoadFloat4(&XMFLOAT4(0,1,0,0)));
  XMStoreFloat4x4(&cbuffer.mirrorViewProj, XMMatrixTranspose(XMMatrixMultiply(reflect, xmViewProj)));

  _ctx->set_cbuffer(vs->find_cbuffer("cbPlane"), 0, ShaderType::kVertexShader, &cbuffer, sizeof(cbuffer));
  _ctx->set_samplers(ps->samplers());
  _ctx->set_shader_resource(rtMirror, ShaderType::kPixelShader);

  _ctx->set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  _ctx->set_vb(_planeVb);
  _ctx->set_ib(_planeIb);
  _ctx->draw_indexed(6, 0, 0);
}

void SplineTest::renderParticles() {

  int w = GRAPHICS.width();
  int h = GRAPHICS.height();

  Technique *technique;
  Shader *vs, *gs, *ps;
  setupTechnique(_ctx, _particle_technique, false, &technique, &vs, &gs, &ps);

  _ctx->set_topology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

  TextureArray arr = { _particle_texture };
  _ctx->set_shader_resources(arr, ShaderType::kPixelShader);

  _ctx->set_samplers(ps->samplers());

  struct {
    XMFLOAT4 cameraPos;
    XMFLOAT4X4 worldView;
    XMFLOAT4X4 proj;
  } cbuffer;

  cbuffer.cameraPos = expand(_cameraPos, 0);
  cbuffer.worldView = transpose(_view);
  cbuffer.proj = transpose(_proj);

  _ctx->set_cbuffer(vs->find_cbuffer("ParticleBuffer"), 0, ShaderType::kVertexShader, &cbuffer, sizeof(cbuffer));
  _ctx->set_cbuffer(gs->find_cbuffer("ParticleBuffer"), 0, ShaderType::kGeometryShader, &cbuffer, sizeof(cbuffer));

  _ctx->set_vb(_particleVb);
  _ctx->draw(cNumParticles, 0);
}

void SplineTest::renderSplines(GraphicsObjectHandle rtMirror) {

  int w = GRAPHICS.width();
  int h = GRAPHICS.height();

  D3D11_MAPPED_SUBRESOURCE staticRes;
  D3D11_MAPPED_SUBRESOURCE dynamicRes;

  _ctx->map(_staticVb, 0, _staticVertCount == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &staticRes);
  _ctx->map(_dynamicVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &dynamicRes);

  VsInput *staticVerts = (VsInput *)staticRes.pData;
  VsInput *dynamicVerts = (VsInput *)dynamicRes.pData;

  int newDynamicVerts = 0;
  int newStaticVerts = 0;
  for (auto it = begin(_splines); it != end(_splines); ++it) {
    DynamicSpline *spline = *it;
    spline->render(staticVerts + _staticVertCount + newStaticVerts, dynamicVerts + newDynamicVerts, &newStaticVerts, &newDynamicVerts);
  }
  _staticVertCount += newStaticVerts;

  _ctx->unmap(_staticVb, 0);
  _ctx->unmap(_dynamicVb, 0);

#pragma pack(push, 1)
  struct {
    XMFLOAT4X4 worldViewProj;
    XMFLOAT4 cameraPos;
    XMFLOAT4 extrudeShape[21];
  } cbuffer;
#pragma pack(pop)

  XMFLOAT4X4 worldViewProj;
  XMMATRIX xmViewProj = XMMatrixMultiply(XMLoadFloat4x4(&_view), XMLoadFloat4x4(&_proj));
  XMStoreFloat4x4(&cbuffer.worldViewProj, XMMatrixTranspose(xmViewProj));
  cbuffer.cameraPos = expand(_cameraPos,0);
  memcpy(cbuffer.extrudeShape, _extrudeShape.data(), sizeof(cbuffer.extrudeShape));

  Technique *technique;
  Shader *vs, *gs, *ps;
  setupTechnique(_ctx, _splineTechnique, false, &technique, &vs, &gs, &ps);

  _ctx->set_topology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

  _ctx->set_cbuffer(vs->find_cbuffer("test"), 0, ShaderType::kVertexShader, &cbuffer, sizeof(cbuffer));
  _ctx->set_cbuffer(gs->find_cbuffer("test"), 0, ShaderType::kGeometryShader, &cbuffer, sizeof(cbuffer));
  _ctx->set_cbuffer(ps->find_cbuffer("test"), 0, ShaderType::kPixelShader, &cbuffer, sizeof(cbuffer));

  _ctx->set_samplers(ps->samplers());
  _ctx->set_shader_resource(_cubeMap, ShaderType::kPixelShader);

  _ctx->set_vb(_staticVb);
  _ctx->draw(_staticVertCount, 0);

  _ctx->set_vb(_dynamicVb);
  _ctx->draw(newDynamicVerts, 0);


  _ctx->set_render_target(rtMirror, true);

  XMMATRIX reflect = XMMatrixReflect(XMLoadFloat4(&XMFLOAT4(0,1,0,0)));
  XMStoreFloat4x4(&cbuffer.worldViewProj, XMMatrixTranspose(XMMatrixMultiply(reflect, xmViewProj)));
  XMStoreFloat4(&cbuffer.cameraPos, XMVector4Transform(XMLoadFloat4(&cbuffer.cameraPos), reflect));
  _ctx->set_cbuffer(vs->find_cbuffer("test"), 0, ShaderType::kVertexShader, &cbuffer, sizeof(cbuffer));
  _ctx->set_cbuffer(gs->find_cbuffer("test"), 0, ShaderType::kGeometryShader, &cbuffer, sizeof(cbuffer));
  _ctx->set_cbuffer(ps->find_cbuffer("test"), 0, ShaderType::kPixelShader, &cbuffer, sizeof(cbuffer));

  _ctx->set_rs(GRAPHICS.find_rasterizer_state("FrontfaceCulling"));
  _ctx->set_vb(_staticVb);
  _ctx->draw(_staticVertCount, 0);

  _ctx->set_vb(_dynamicVb);
  _ctx->draw(newDynamicVerts, 0);

  _ctx->unset_render_targets(0, 1);

}

bool SplineTest::render() {
  ADD_PROFILE_SCOPE();
  _ctx->begin_frame();

  int w = GRAPHICS.width();
  int h = GRAPHICS.height();

  KASSERT(w && h);

  XMFLOAT4X4 worldViewProj;
  XMMATRIX xWorldViewProj = XMMatrixMultiply(XMLoadFloat4x4(&_view), XMLoadFloat4x4(&_proj));
  XMStoreFloat4x4(&worldViewProj, XMMatrixMultiply(XMLoadFloat4x4(&_view), XMLoadFloat4x4(&_proj)));
  XMFLOAT4 clipSpaceLightPos(0,100,200,1);
  XMStoreFloat4(&clipSpaceLightPos, XMVector3TransformCoord(XMLoadFloat4(&clipSpaceLightPos), xWorldViewProj));
  // divide by w -> ndc
  clipSpaceLightPos.x /= clipSpaceLightPos.w;
  clipSpaceLightPos.y /= clipSpaceLightPos.w;
  clipSpaceLightPos.z /= clipSpaceLightPos.w;

  // convert to texture space
  struct {
    XMFLOAT4 ssLight;
    XMFLOAT4 texLight;
    XMFLOAT2 screenSize;
  } cbufferLight;

  cbufferLight.texLight.x = (0.5f + 0.5f * clipSpaceLightPos.x);
  cbufferLight.texLight.y = (1.0f - (0.5f + 0.5f * clipSpaceLightPos.y));
  cbufferLight.ssLight.x = cbufferLight.texLight.x * w;
  cbufferLight.ssLight.y = cbufferLight.texLight.y * h;

  cbufferLight.screenSize = XMFLOAT2((float)w, (float)h);

  ScopedRt rtColor(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv | Graphics::kCreateDepthBuffer, "rtColor");
  ScopedRt rtOcclude(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv | Graphics::kCreateDepthBuffer, "rtOcclude");

  {
    Technique *technique;
    Shader *ps, *vs;
    setupTechnique(_ctx, _gradient_technique, true, &technique, &vs, nullptr, &ps);
    _ctx->set_cbuffer(ps->find_cbuffer("WorldToScreenSpace"), 0, ShaderType::kPixelShader, &cbufferLight, sizeof(cbufferLight));

    _ctx->set_render_target(rtColor, true);
    _ctx->draw_indexed(technique->index_count(), 0, 0);

    //_ctx->set_render_target(rtOcclude, true);
    //_ctx->draw_indexed(technique->index_count(), 0, 0);
  }


  GraphicsObjectHandle rts[] = { rtColor, rtOcclude };
  bool clear[] = { false, true };
  _ctx->set_render_targets(rts, clear, 2);

  int w4 = w/4;
  int h4 = h/4;
  ScopedRt rtMirror(w4, h4, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv | Graphics::kCreateDepthBuffer, "rtMirror");
  renderSplines(rtMirror);

  _ctx->set_render_target(rtColor, false);
  renderParticles();

  ScopedRt rtMirrorBlur1(w4, h4, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv | Graphics::kCreateUav, "rtMirrorBlur1");
  ScopedRt rtMirrorBlur2(w4, h4, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv | Graphics::kCreateUav, "rtMirrorBlur2");
  _blur.do_blur(4, rtMirror, rtMirrorBlur1, rtMirrorBlur2, w4, h4, _ctx);

  renderPlane(rtMirrorBlur2);

  int w2 = w/2;
  int h2 = h/2;
  ScopedRt rtDownscale(w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv, "rtDownscale");
  ScopedRt rtBlur1(w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv | Graphics::kCreateUav, "rtBlur1");
  ScopedRt rtBlur2(w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, Graphics::kCreateSrv | Graphics::kCreateUav, "rtBlur2");

  post_process(rtOcclude, rtDownscale, _scale);
  _blur.do_blur(10, rtDownscale, rtBlur1, rtBlur2, w2, h2, _ctx);


  {
    Technique *technique;
    Shader *ps, *vs;
    setupTechnique(_ctx, _compose_technique, true, &technique, &vs, nullptr, &ps);

    _ctx->set_cbuffer(ps->find_cbuffer("WorldToScreenSpace"), 0, ShaderType::kPixelShader, &cbufferLight, sizeof(cbufferLight));

    _ctx->set_default_render_target(false);
    TextureArray arr = { rtColor, rtBlur2 };
    _ctx->set_samplers(ps->samplers());
    _ctx->set_shader_resources(arr, ShaderType::kPixelShader);

    _ctx->draw_indexed(technique->index_count(), 0, 0);
    _ctx->unset_shader_resource(0, 2, ShaderType::kPixelShader);
  }

  _ctx->end_frame();

  return true;
}

bool SplineTest::close() {
  return true;
}

