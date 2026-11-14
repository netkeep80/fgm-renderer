#include "rvi-v02.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace rvi {
namespace {
constexpr double kDomain = 2.0;
constexpr double kProbe = 1e-7;
constexpr double kBoundaryTolerance = 1e-10;

Vec add(Vec a, Vec b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec sub(Vec a, Vec b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec scale(Vec a, double k) { return {a.x*k, a.y*k, a.z*k}; }
double dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
double axis(Vec v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

bool in_box(Vec p, Vec lo, Vec hi) {
  return p.x >= lo.x && p.x <= hi.x &&
         p.y >= lo.y && p.y <= hi.y &&
         p.z >= lo.z && p.z <= hi.z;
}
bool inside(const Body& b, Vec p) {
  if (b.kind == Kind::sphere) {
    const Vec q = sub(p, b.center);
    return dot(q,q) <= b.radius*b.radius;
  }
  if (!in_box(p,b.lower,b.upper)) return false;
  return b.kind != Kind::panel || !in_box(p,b.hole_lower,b.hole_upper);
}
bool slab(const Ray& ray, Vec lo, Vec hi, double& enter, double& leave) {
  enter = -std::numeric_limits<double>::infinity();
  leave = std::numeric_limits<double>::infinity();
  for (int a=0;a<3;++a) {
    const double o=axis(ray.origin,a), d=axis(ray.direction,a);
    const double l=axis(lo,a), h=axis(hi,a);
    if (d == 0) {
      if (o < l || o > h) return false;
      continue;
    }
    double t0=(l-o)/d, t1=(h-o)/d;
    if (t0>t1) std::swap(t0,t1);
    enter=std::max(enter,t0);
    leave=std::min(leave,t1);
    if (enter>leave) return false;
  }
  return true;
}
bool actual_interface(const Scene& scene,const Ray& ray,double t,std::string& material) {
  if (!std::isfinite(t) || t < 0) return false;
  const auto before=semantic_material(scene,point_on(ray,t-kProbe));
  const auto after=semantic_material(scene,point_on(ray,t+kProbe));
  if (before==after) return false;
  material=after.empty()?before:after;
  return !material.empty();
}
int owner(double value, int divisions) {
  const double unit=(value+kDomain)*divisions/(2*kDomain);
  if (unit < -kBoundaryTolerance || unit > divisions+kBoundaryTolerance)
    return -1;
  const double nearest=std::round(unit);
  const double normalized=std::abs(unit-nearest)<=kBoundaryTolerance ? nearest : unit;
  return std::clamp(static_cast<int>(std::floor(normalized)),0,divisions-1);
}
bool owns(const Ray& ray,double t,int d,int ix,int iy,int iz) {
  const Vec p=point_on(ray,t);
  return owner(p.x,d)==ix && owner(p.y,d)==iy && owner(p.z,d)==iz;
}
void roots(const Body& body,const Ray& ray,std::vector<double>& candidates) {
  if (body.kind==Kind::sphere) {
    const Vec delta=sub(ray.origin,body.center);
    const double q=dot(delta,ray.direction);
    const double c=dot(delta,delta)-body.radius*body.radius;
    const double discriminant=q*q-c;
    if (discriminant < 0) return;
    const double root=std::sqrt(discriminant);
    candidates.push_back(-q-root);
    candidates.push_back(-q+root);
    return;
  }
  double near=0,far=0;
  if (slab(ray,body.lower,body.upper,near,far)) {
    candidates.push_back(near);
    candidates.push_back(far);
  }
  if (body.kind==Kind::panel && slab(ray,body.hole_lower,body.hole_upper,near,far)) {
    candidates.push_back(near);
    candidates.push_back(far);
  }
}
} // namespace

Vec point_on(const Ray& ray,double t) {
  return add(ray.origin,scale(ray.direction,t));
}
Ray global_ray(const Camera& camera,const Sample& sample) {
  if (camera.orientation!=0 && camera.orientation!=1)
    throw std::invalid_argument("E_CAMERA_ORIENTATION");
  if (!(camera.pixel_scale>0) || camera.width<1 || camera.height<1 ||
      sample.x<0 || sample.x>=camera.width || sample.y<0 || sample.y>=camera.height ||
      !(sample.offset_x>=0 && sample.offset_x<=1 &&
        sample.offset_y>=0 && sample.offset_y<=1))
    throw std::invalid_argument("E_CAMERA_SAMPLE");
  const double u=(sample.x+sample.offset_x-camera.width/2.0)*camera.pixel_scale;
  const double v=(camera.height/2.0-sample.y-sample.offset_y)*camera.pixel_scale;
  return camera.orientation==0 ? Ray{{u,v,4},{0,0,-1}} : Ray{{4,u,v},{-1,0,0}};
}
std::string semantic_material(const Scene& scene,Vec point) {
  int priority=std::numeric_limits<int>::min();
  std::string winner, material;
  for (const auto& b:scene.bodies) {
    if (inside(b,point) &&
        (b.priority>priority || (b.priority==priority && (winner.empty() || b.id<winner)))) {
      winner=b.id;
      priority=b.priority;
      material=b.material;
    }
  }
  return material;
}
Hit compose(Hit left,Hit right) {
  if (left.found && (!std::isfinite(left.depth) || left.depth<0))
    throw std::invalid_argument("E_NONFINITE_DEPTH");
  if (right.found && (!std::isfinite(right.depth) || right.depth<0))
    throw std::invalid_argument("E_NONFINITE_DEPTH");
  if (!left.found) return right;
  if (!right.found) return left;
  // No epsilon: the lexicographic key induces a strict associative min.
  if (right.depth<left.depth || (right.depth==left.depth && right.fragment<left.fragment))
    return right;
  return left;
}
Hit render_cell(const Scene& scene,const Ray& ray,
                int divisions,int ix,int iy,int iz,int* root_count,bool ignore_owner) {
  if (divisions!=2 && divisions!=4) throw std::invalid_argument("E_LEVEL");
  if (ix<0 || ix>=divisions || iy<0 || iy>=divisions || iz<0 || iz>=divisions)
    throw std::invalid_argument("E_CUBE_INDEX");
  const double step=2*kDomain/divisions;
  const Vec lo={-kDomain+ix*step,-kDomain+iy*step,-kDomain+iz*step};
  const Vec hi={lo.x+step,lo.y+step,lo.z+step};
  double enter=0,leave=0;
  if (!slab(ray,lo,hi,enter,leave) || leave<0) return {};
  enter=std::max(enter,0.0);
  Hit best;
  for (const auto& b:scene.bodies) {
    std::vector<double> candidates;
    roots(b,ray,candidates);
    for (const double t:candidates) {
      if (t<enter-kBoundaryTolerance || t>leave+kBoundaryTolerance) continue;
      std::string material;
      if (!actual_interface(scene,ray,t,material)) continue;
      if (!ignore_owner && !owns(ray,t,divisions,ix,iy,iz)) continue;
      if (root_count) ++*root_count;
      best=compose(std::move(best),Hit{true,t,b.id,material});
    }
  }
  return best;
}
Hit render_recursive(const Scene& scene,const Ray& ray,int levels,bool reverse) {
  if (levels!=1 && levels!=2) throw std::invalid_argument("E_LEVEL");
  const int n=1<<levels;
  std::array<Hit,8> parent_hits{};
  for (int i=0;i<n*n*n;++i) {
    const int k=reverse?n*n*n-1-i:i;
    const int ix=k/(n*n),iy=(k/n)%n,iz=k%n;
    const Hit h=render_cell(scene,ray,n,ix,iy,iz);
    const int parent=levels==1?0:(ix/2)*4+(iy/2)*2+iz/2;
    parent_hits[static_cast<std::size_t>(parent)]=
      compose(std::move(parent_hits[static_cast<std::size_t>(parent)]),h);
  }
  Hit total;
  for (const auto& h:parent_hits) total=compose(std::move(total),h);
  return total;
}
Pixel render_pixel(const Scene& scene, const Camera& camera, int x, int y,
                   int levels, const std::vector<std::array<double,2>>& offsets) {
  if (offsets.empty()) throw std::invalid_argument("E_EMPTY_SAMPLES");
  Pixel result;
  for (const auto& offset:offsets) {
    const Ray ray=global_ray(camera,{x,y,offset[0],offset[1]});
    const Hit hit=render_recursive(scene,ray,levels);
    if (hit.found) result.coverage += 1.0/static_cast<double>(offsets.size());
    result.samples.push_back(hit);
  }
  return result;
}
std::vector<Scene> load_scenes(const std::string& path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("E_FIXTURE_OPEN");
  std::vector<Scene> scenes;
  Scene active;
  std::string token;
  while (stream>>token) {
    if (token=="scene") {
      if (!active.id.empty()) throw std::runtime_error("E_FIXTURE_NESTING");
      stream>>active.id;
    } else if (token=="end") {
      if (active.id.empty() || active.bodies.empty()) throw std::runtime_error("E_FIXTURE_EMPTY");
      scenes.push_back(std::move(active));
      active=Scene{};
    } else {
      if (active.id.empty()) throw std::runtime_error("E_FIXTURE_BODY");
      Body body;
      if (token=="sphere") {
        body.kind=Kind::sphere;
        stream>>body.id>>body.material>>body.priority>>
          body.center.x>>body.center.y>>body.center.z>>body.radius;
        if (!(body.radius>0)) throw std::runtime_error("E_SPHERE");
      } else if (token=="box" || token=="panel") {
        body.kind=token=="box"?Kind::box:Kind::panel;
        stream>>body.id>>body.material>>body.priority>>
          body.lower.x>>body.lower.y>>body.lower.z>>
          body.upper.x>>body.upper.y>>body.upper.z;
        if (body.kind==Kind::panel) {
          stream>>body.hole_lower.x>>body.hole_lower.y>>body.hole_lower.z>>
            body.hole_upper.x>>body.hole_upper.y>>body.hole_upper.z;
        }
        if (!(body.lower.x<body.upper.x && body.lower.y<body.upper.y && body.lower.z<body.upper.z))
          throw std::runtime_error("E_BOX");
      } else throw std::runtime_error("E_FIXTURE_TOKEN");
      if (!stream || body.id.empty() || body.material.empty()) throw std::runtime_error("E_FIXTURE_INPUT");
      active.bodies.push_back(std::move(body));
    }
  }
  if (!active.id.empty() || scenes.size()!=10) throw std::runtime_error("E_FIXTURE_COUNT");
  return scenes;
}
} // namespace rvi
