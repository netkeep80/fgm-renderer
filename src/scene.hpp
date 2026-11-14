#pragma once

#include "scene-size.hpp"

#include <span>
#include <string_view>

void sample_scene(std::string_view package, std::span<float, scene_size * scene_size> field);
