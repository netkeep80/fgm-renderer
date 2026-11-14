#include "scene.hpp"

#include <fgm/evaluator.hpp>

void sample_scene(std::string_view package, std::span<float, scene_size * scene_size> field) {
  const auto evaluator = fgm::Evaluator::from_package(package);
  for (std::size_t y = 0; y < scene_size; ++y) {
    for (std::size_t x = 0; x < scene_size; ++x) {
      const auto coordinate = [](std::size_t pixel) {
        return (static_cast<double>(pixel) + 0.5) * 3.2 / scene_size - 1.6;
      };
      field[y * scene_size + x] = static_cast<float>(
          evaluator.sample_local({coordinate(x), coordinate(y), 0.25}).value);
    }
  }
}
