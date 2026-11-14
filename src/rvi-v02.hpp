#pragma once
#include <array>
#include <string>
#include <vector>

namespace rvi {
struct Vec { double x = 0, y = 0, z = 0; };
struct Ray { Vec origin, direction; };
struct Camera {
  int orientation = 0;
  double pixel_scale = 0.1;
  int width = 33, height = 33;
};
struct Sample { int x = 0, y = 0; double offset_x = 0.5, offset_y = 0.5; };
enum class Kind { sphere, box, panel };
struct Body {
  Kind kind = Kind::box;
  std::string id, material;
  int priority = 0;
  Vec lower, upper;      // box and panel
  Vec center;            // sphere
  double radius = 0;    // sphere
  Vec hole_lower, hole_upper; // panel subtraction
};
struct Scene {
  std::string id;
  std::vector<Body> bodies;
};
struct Hit {
  bool found = false;
  double depth = 0;
  std::string fragment, material;
};
struct Pixel {
  std::vector<Hit> samples;
  double coverage = 0.0;
};
Vec point_on(const Ray& ray, double t);
Ray global_ray(const Camera& camera, const Sample& sample);
std::vector<Scene> load_scenes(const std::string& path);
std::string semantic_material(const Scene& scene, Vec point);
Hit compose(Hit left, Hit right);
Hit render_cell(const Scene& scene, const Ray& ray,
                int divisions, int ix, int iy, int iz,
                int* root_count = nullptr, bool ignore_owner = false);
Hit render_recursive(const Scene& scene, const Ray& ray,
                     int levels, bool reverse = false);
Pixel render_pixel(const Scene& scene, const Camera& camera, int x, int y,
                   int levels, const std::vector<std::array<double,2>>& offsets);
} // namespace rvi
