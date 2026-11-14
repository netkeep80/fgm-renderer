#include "../src/rvi-v02.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
// Independent direct oracle: evaluate whole-object geometric planes and
// quadratic roots without calling render_cell or compose.
rvi::Vec difference(rvi::Vec a,rvi::Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
double inner(rvi::Vec a,rvi::Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
double coordinate(rvi::Vec p,int d) { return d==0?p.x:(d==1?p.y:p.z); }
struct Event { double t; std::string source; };
std::vector<Event> oracle_events(const rvi::Scene& scene,const rvi::Ray& ray) {
  std::vector<Event> events;
  for (const auto& b:scene.bodies) {
    if (b.kind==rvi::Kind::sphere) {
      const rvi::Vec m=difference(ray.origin,b.center);
      const double a=inner(ray.direction,ray.direction), bb=2*inner(m,ray.direction);
      const double c=inner(m,m)-b.radius*b.radius, disc=bb*bb-4*a*c;
      if (disc>=0) {
        events.push_back({(-bb-std::sqrt(disc))/(2*a),b.id});
        events.push_back({(-bb+std::sqrt(disc))/(2*a),b.id});
      }
    } else {
      const auto planes=[&](rvi::Vec lo,rvi::Vec hi) {
        for (int axis=0;axis<3;++axis) {
          const double d=coordinate(ray.direction,axis);
          if (d==0) continue;
          for (double v:{coordinate(lo,axis),coordinate(hi,axis)}) {
            const double t=(v-coordinate(ray.origin,axis))/d;
            if (t<0) continue;
            const auto point=rvi::point_on(ray,t);
            bool inside_face=true;
            for (int other=0;other<3;++other) {
              if (other==axis) continue;
              const double q=coordinate(point,other);
              if (q<coordinate(lo,other)-1e-12 || q>coordinate(hi,other)+1e-12)
                inside_face=false;
            }
            if (inside_face) events.push_back({t,b.id});
          }
        }
      };
      planes(b.lower,b.upper);
      if (b.kind==rvi::Kind::panel) planes(b.hole_lower,b.hole_upper);
    }
  }
  return events;
}
rvi::Hit reference(const rvi::Scene& scene,const rvi::Ray& ray) {
  rvi::Hit best;
  for (const auto& event:oracle_events(scene,ray)) {
    if (event.t<0 || !std::isfinite(event.t)) continue;
    const auto a=rvi::semantic_material(scene,rvi::point_on(ray,event.t-1e-7));
    const auto b=rvi::semantic_material(scene,rvi::point_on(ray,event.t+1e-7));
    if (a==b) continue;
    const auto material=b.empty()?a:b;
    if (material.empty()) continue;
    const rvi::Hit current{true,event.t,event.source,material};
    if (!best.found || current.depth<best.depth ||
        (current.depth==best.depth && current.fragment<best.fragment)) best=current;
  }
  return best;
}
bool equal(const rvi::Hit& a,const rvi::Hit& b,double* delta=nullptr) {
  if (a.found!=b.found) return false;
  if (!a.found) return true;
  if (a.fragment!=b.fragment || a.material!=b.material || !std::isfinite(b.depth)) return false;
  const double e=std::abs(a.depth-b.depth);
  if (delta) *delta=e;
  return e<=1e-9*std::max(1.0,std::abs(a.depth));
}
const rvi::Scene& named(const std::vector<rvi::Scene>& scenes,const std::string& name) {
  for (const auto& s:scenes) if (s.id==name) return s;
  throw std::runtime_error("missing scene "+name);
}
rvi::Ray center() {return rvi::global_ray({0,0.1,33,33},{16,16,0.5,0.5});}
uint64_t digest(uint64_t h,const std::string& s) {
  for (unsigned char c:s) {h^=c;h*=1099511628211ull;} return h;
}
std::string hex(uint64_t x) {std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<x;return s.str();}
struct Negative {std::string name;bool detected;};
std::vector<Negative> targeted_checks(const std::vector<rvi::Scene>& scenes) {
  std::vector<Negative> checks;
  const auto s=named(scenes,"sphere");
  const auto ground=reference(s,center());
  auto cube_entry=ground;cube_entry.depth=2.0;
  checks.push_back({"cube_entry_as_depth",!equal(ground,cube_entry)});
  bool phase=false;
  for (int x=0;x<33;++x) {
    const rvi::Camera c{0,0.16,33,33};
    const auto a=reference(s,rvi::global_ray(c,{x,16,0.5,0.5}));
    const auto b=reference(s,rvi::global_ray(c,{x,16,0.9,0.5}));
    if (!equal(a,b)) {phase=true;break;}
  }
  checks.push_back({"local_raster_phase",phase});
  int duplicates=0;
  const auto edges=named(scenes,"box_edges");
  for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)
    (void)rvi::render_cell(edges,center(),4,i,j,k,&duplicates,true);
  int owned=0;
  for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)
    (void)rvi::render_cell(edges,center(),4,i,j,k,&owned,false);
  checks.push_back({"shared_face_double_count",duplicates>owned && owned==2});
  const auto overlap=named(scenes,"projection_overlap");
  const auto front=reference(overlap,center());
  checks.push_back({"priority_as_depth",front.found && front.fragment=="near_sphere" &&
       !equal(front,{true,4.5,"far_box","gold"})});
  const auto subpixel=named(scenes,"subpixel");
  const auto l=reference(subpixel,rvi::global_ray({0,0.1,33,33},{16,16,0.25,0.5}));
  const auto r=reference(subpixel,rvi::global_ray({0,0.1,33,33},{16,16,0.75,0.5}));
  const auto pixel=rvi::render_pixel(subpixel,{0,0.1,33,33},16,16,2,
                                    {{0.25,0.5},{0.75,0.5}});
  checks.push_back({"early_coverage_average",l.found && r.found && l.fragment!=r.fragment &&
    (int(l.found)+int(r.found))/2.0!=0.5 &&
    pixel.coverage==1.0 && pixel.samples.size()==2 &&
    equal(pixel.samples[0],l) && equal(pixel.samples[1],r)});
  const auto tie=reference(named(scenes,"equal_depth"),center());
  checks.push_back({"reverse_tie",tie.found && tie.fragment=="a_tie" &&
      !equal(tie,{true,tie.depth,"z_tie",tie.material})});
  checks.push_back({"wrong_material_after_tie",tie.found && tie.material=="metal" &&
      !equal(tie,{true,tie.depth,"a_tie","stone"})});
  rvi::Hit last;
  for(int k=63;k>=0;--k) {
    const auto h=rvi::render_cell(overlap,center(),4,k/16,(k/4)%4,k%4);
    if(h.found)last=h;
  }
  checks.push_back({"last_wins_incorrect_minimum",!equal(front,last)});
  bool rejects=false;
  try {(void)rvi::compose({}, {true,std::numeric_limits<double>::quiet_NaN(),"bad","bad"});}
  catch(const std::invalid_argument&) {rejects=true;}
  checks.push_back({"nan_depth_rejection",rejects});
  const auto continuous=named(scenes,"crossing_inside");
  checks.push_back({"internal_cube_not_a_surface",
                    !rvi::render_cell(continuous,center(),4,2,2,2).found});
  return checks;
}
bool composition_laws() {
  const std::vector<rvi::Hit> inputs = {
    {}, {true,2.0,"z","stone"}, {true,1.0,"b","steel"}, {true,1.0,"a","wood"}
  };
  const auto identical=[](const rvi::Hit& a,const rvi::Hit& b) {
    return a.found==b.found &&
      (!a.found || (a.depth==b.depth && a.fragment==b.fragment && a.material==b.material));
  };
  for(const auto& a:inputs)for(const auto& b:inputs)for(const auto& c:inputs) {
    const auto left=rvi::compose(rvi::compose(a,b),c);
    const auto right=rvi::compose(a,rvi::compose(b,c));
    if(!identical(left,right))return false;
  }
  std::array<int,4> order{0,1,2,3};
  do {
    rvi::Hit h;
    for(int i:order)h=rvi::compose(h,inputs[static_cast<std::size_t>(i)]);
    if(!identical(h,{true,1.0,"a","wood"}))return false;
  }while(std::next_permutation(order.begin(),order.end()));
  return true;
}
bool verify_semantic_boundaries(const std::vector<rvi::Scene>& scenes) {
  const auto sphere=named(scenes,"sphere");
  const auto inside=rvi::semantic_at(sphere,{0.0,0.0,0.0});
  const auto on=rvi::semantic_at(sphere,{0.8,0.0,0.0});
  const auto outside=rvi::semantic_at(sphere,{1.0,0.0,0.0});
  const auto box=named(scenes,"box_edges");
  const auto box_face=rvi::semantic_at(box,{0.0,0.5,0.0});
  const auto box_in=rvi::semantic_at(box,{0.5,0.5,0.0});
  const auto panel=named(scenes,"panel_hole");
  const auto void_center=rvi::semantic_at(panel,{0,0,0});
  const auto void_wall=rvi::semantic_at(panel,{0.35,0,0});
  const auto actual_wall=rvi::semantic_at(panel,{0.6,0,0});
  return inside.material=="wood" && inside.contributor=="sphere_a" &&
      on.material.empty() && on.contributor.empty() &&
      outside.material.empty() && box_face.material.empty() &&
      box_in.material=="steel" && void_center.material.empty() &&
      void_wall.material.empty() && actual_wall.material=="steel";
}
bool verify_c02_exact_ownership() {
  const auto owns=[](double x,int depth,int index) {
    return rvi::cell_owner(x,depth)==index;
  };
  const double zero=0, left=std::nextafter(zero,-1.0), right=std::nextafter(zero,1.0);
  const double low=-2, high=2, below=std::nextafter(low,-3.0);
  const double near_high=std::nextafter(high,1.0);
  return owns(low,2,0) && owns(low,4,0) &&
         owns(std::nextafter(low,-1.0),2,0) &&
         owns(below,2,-1) && owns(below,4,-1) &&
         owns(left,2,0) && owns(zero,2,1) && owns(right,2,1) &&
         owns(left,4,1) && owns(zero,4,2) && owns(right,4,2) &&
         owns(-1.0,4,1) && owns(std::nextafter(-1.0,-2.0),4,0) &&
         owns(std::nextafter(-1.0,0.0),4,1) &&
         owns(1.0,4,3) && owns(std::nextafter(1.0,-1.0),4,2) &&
         owns(near_high,2,1) && owns(near_high,4,3) &&
         owns(high,2,-1) && owns(high,4,-1);
}
bool verify_compose_contract() {
  const rvi::Hit zero{true,-0.0,"a","wood"};
  const auto norm=rvi::compose({},zero);
  if (!norm.found || std::signbit(norm.depth)) return false;
  bool rejects=false;
  try {
    (void)rvi::compose({true,1.0,"same","wood"},
                       {true,1.0,"same","metal"});
  } catch (const std::invalid_argument& e) {
    rejects=std::string(e.what())=="E_FRAGMENT_CONFLICT";
  }
  return rejects;
}
} // namespace

int main(int argc,char** argv) {
  try {
    if(argc!=3)throw std::runtime_error("usage: v02-verify fixtures-file hits.csv");
    std::ofstream trace(argv[2],std::ios::binary);
    if(!trace)throw std::runtime_error("E_TRACE_FILE");
    trace<<std::setprecision(17)
         <<"scene,view,scale,x,y,offset_x,offset_y,mode,hit,depth,fragment,material\n";
    const auto scenes=rvi::load_scenes(argv[1]);
    std::ostringstream reports;
    reports<<std::setprecision(17);
    std::size_t count=0,total=0,failed=0;
    double worst=0;
    for(const auto& scene:scenes)for(int view=0;view<2;++view)for(double scale:{0.1,0.2}) {
      const rvi::Camera cam{view,scale,33,33};
      const std::vector<std::array<double,2>> offsets =
        scene.id=="subpixel" ? std::vector<std::array<double,2>>{{0.25,0.5},{0.75,0.5}} :
        std::vector<std::array<double,2>>{{0.5,0.5}};
      std::size_t samples=0,errors=0;double local_worst=0;
      uint64_t hash=14695981039346656037ull;
      for(int y=0;y<33;++y)for(int x=0;x<33;++x)for(const auto offset:offsets) {
        const auto ray=rvi::global_ray(cam,{x,y,offset[0],offset[1]});
        const auto truth=reference(scene,ray);
        const auto a=rvi::render_recursive(scene,ray,1);
        const auto b=rvi::render_recursive(scene,ray,2);
        const auto swapped=rvi::render_recursive(scene,ray,2,true);
        const auto emit=[&](const char* mode,const rvi::Hit& hit) {
          trace<<scene.id<<","<<view<<","<<scale<<","<<x<<","<<y<<","
               <<offset[0]<<","<<offset[1]<<","<<mode<<","
               <<(hit.found?"1":"0")<<",";
          if(hit.found)trace<<hit.depth<<","<<hit.fragment<<","<<hit.material;
          else trace<<",,";
          trace<<"\n";
        };
        emit("direct",truth);
        emit("eight",a);
        emit("sixtyfour",b);
        emit("permuted",swapped);
        double e1=0,e2=0,e3=0;
        const bool ok=equal(truth,a,&e1)&&equal(truth,b,&e2)&&equal(truth,swapped,&e3);
        local_worst=std::max({local_worst,e1,e2,e3});
        hash=digest(hash,std::to_string(x)+":"+std::to_string(y)+":"+
          std::to_string(offset[0])+":"+(truth.found?truth.fragment+"|"+truth.material:"empty"));
        ++samples;
        if(!ok) {
          ++errors;
          if(errors<=3)std::cerr<<"РАСХОЖДЕНИЕ "<<scene.id<<" camera="<<view<<" scale="<<scale
            <<" sample="<<x<<","<<y<<" offset="<<offset[0]<<" ref="<<truth.fragment<<" "<<truth.depth
            <<" 8="<<a.fragment<<" "<<a.depth<<" 64="<<b.fragment<<" "<<b.depth<<"\n";
        }
      }
      if(count)reports<<",";
      reports<<"{\"scene\":\""<<scene.id<<"\",\"camera\":"<<view<<",\"scale\":"<<scale
         <<",\"samples\":"<<samples<<",\"mismatches\":"<<errors
         <<",\"max_depth_error\":"<<local_worst<<",\"identity_digest\":\""<<hex(hash)<<"\"}";
      ++count;total+=samples;failed+=errors;worst=std::max(worst,local_worst);
    }
    const bool associative=composition_laws();
    const bool semantic=verify_semantic_boundaries(scenes);
    const bool ownership=verify_c02_exact_ownership();
    const bool hit_contract=verify_compose_contract();
    const auto neg=targeted_checks(scenes);
    std::size_t detected=0;
    std::ostringstream negative;
    for(std::size_t i=0;i<neg.size();++i) {
      if(i)negative<<",";
      negative<<"{\"mutation\":\""<<neg[i].name<<"\",\"detected\":"
        <<(neg[i].detected?"true":"false")<<"}";
      if(neg[i].detected)++detected;
    }
    const bool ok=failed==0&&count==40&&detected==neg.size()&&associative&&semantic&&ownership&&hit_contract;
    std::cout<<std::setprecision(17)
      <<"{\"schema\":\"fgm-v02-report-1\",\"ok\":"<<(ok?"true":"false")
      <<",\"case_count\":"<<count<<",\"samples\":"<<total
      <<",\"mismatches\":"<<failed<<",\"max_depth_error\":"<<worst
      <<",\"semantic_boundaries\":"<<(semantic?"true":"false")
      <<",\"c02_ownership\":"<<(ownership?"true":"false")
      <<",\"hit_contract\":"<<(hit_contract?"true":"false")
      <<",\"composition_laws\":"<<(associative?"true":"false")
      <<",\"targeted_checks_passed\":"<<detected<<",\"targeted_checks_total\":"<<neg.size()
      <<",\"cases\":["<<reports.str()<<"],\"targeted_checks\":["<<negative.str()<<"]}\n";
    return ok?0:2;
  }catch(const std::exception& error){std::cerr<<"V02 отказ: "<<error.what()<<"\n";return 3;}
}
