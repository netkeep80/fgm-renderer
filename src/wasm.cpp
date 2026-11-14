#include "scene.hpp"

extern "C" int fgm_scene_size() {
  return scene_size;
}

extern "C" int fgm_sample_scene(const char* bytes, int size, float* output) {
  if (!bytes || size <= 0 || !output) return 1;
  try {
    sample_scene({bytes, static_cast<std::size_t>(size)},
                 std::span<float, scene_size * scene_size>(output, scene_size * scene_size));
    return 0;
  } catch (...) {
    return 1;
  }
}
