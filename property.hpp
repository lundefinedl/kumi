#pragma once

namespace PropertyType {
  // LOWORD is type, HIWORD is length of array
  enum Enum {
    kUnknown,
    kFloat,
    kFloat2,
    kFloat3,
    kFloat4,
    kColor,
    kFloat4x4,
    kTexture2d,
    kSampler,
    kInt,
  };

  inline int len(Enum type) {
    int num_elems = max(1, HIWORD(type));

    switch (LOWORD(type)) {
      case kFloat: return num_elems * sizeof(float);
      case kFloat2: return num_elems * sizeof(XMFLOAT2);
      case kFloat3: return num_elems * sizeof(XMFLOAT3);
      case kFloat4: return num_elems * sizeof(XMFLOAT4);
      case kFloat4x4: return num_elems * sizeof(XMFLOAT4X4);
      case kInt: return num_elems * sizeof(int);
    default:
      return 0;
    }
  }
}
