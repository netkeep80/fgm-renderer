#include "../src/rvi-v02.hpp"
#include <fgm/evaluator.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json=nlohmann::json;
using rvi::Body;
using rvi::Vec;

Json v(Vec p) {return Json::array({p.x,p.y,p.z});}
Vec mid(Vec a,Vec b) {return {(a.x+b.x)/2,(a.y+b.y)/2,(a.z+b.z)/2};}
Vec half(Vec a,Vec b) {return {(b.x-a.x)/2,(b.y-a.y)/2,(b.z-a.z)/2};}
Json constant_vec(const char* name,Vec p) {
  return Json{{"id",name},{"op","constant"},{"type","vec3"},{"value",v(p)}};
}
Json translate(const char* name,const char* field,const char* offset) {
  return Json{{"id",name},{"op","translate"},{"field",field},{"offset",offset}};
}
Json box(const char* name,const char* radius) {
  return Json{{"id",name},{"op","box"},{"halfSize",radius}};
}
Json field_for(const Body& b) {
  Json nodes=Json::array();
  std::string root;
  if (b.kind==rvi::Kind::sphere) {
    nodes.push_back({{"id","radius"},{"op","constant"},{"type","length"},{"value",b.radius}});
    nodes.push_back({{"id","sphere"},{"op","sphere"},{"radius","radius"}});
    nodes.push_back(constant_vec("offset",b.center));
    nodes.push_back(translate("shape","sphere","offset"));
    root="shape";
  } else {
    nodes.push_back(constant_vec("size",half(b.lower,b.upper)));
    nodes.push_back(box("outer","size"));
    nodes.push_back(constant_vec("offset",mid(b.lower,b.upper)));
    nodes.push_back(translate("outer_shift","outer","offset"));
    root="outer_shift";
    if (b.kind==rvi::Kind::panel) {
      nodes.push_back(constant_vec("hole_size",half(b.hole_lower,b.hole_upper)));
      nodes.push_back(box("hole","hole_size"));
      nodes.push_back(constant_vec("hole_offset",mid(b.hole_lower,b.hole_upper)));
      nodes.push_back(translate("hole_shift","hole","hole_offset"));
      nodes.push_back({{"id","cut"},{"op","subtract"},
                       {"left","outer_shift"},{"right","hole_shift"}});
      root="cut";
    }
  }
  return Json{{"op","program"},{"parameters",Json::object()},
    {"program",Json{{"version","fgm-0"},{"nodes",nodes},{"root",root}}}};
}
Json package_for(const rvi::Scene& scene) {
  Json contributions=Json::array();
  for(const auto& b:scene.bodies) {
    contributions.push_back({{"id",b.id},{"material",b.material},
                             {"priority",b.priority},{"field",field_for(b)}});
  }
  return Json{{"format","fgm-package/1"},{"interpreter","fgm-0/1"},
    {"generation",Json{{"algorithm","v02_semantic_witness"},
      {"version","1.0.0"},{"resources",Json::object()},{"parameters",Json::object()}}},
    {"model",Json{{"contributions",contributions},
      {"seed",std::string(64,'0')},{"volumes",Json::object()}}}};
}
} // namespace

int main(int argc,char** argv) {
  try {
    if(argc!=2) throw std::runtime_error("E_WITNESS_ARGS");
    const auto scenes=rvi::load_scenes(argv[1]);
    std::size_t comparisons=0,boundaries=0;
    for(const auto& scene:scenes) {
      const Json pkg=package_for(scene);
      const auto reference=fgm::Evaluator::from_package(pkg.dump());
      const auto check=[&](Vec p,bool boundary) {
        const auto expected=reference.sample_local({p.x,p.y,p.z});
        const auto observed=rvi::semantic_at(scene,p);
        const auto material=expected.material.value_or("");
        const auto id=expected.contributor.value_or("");
        if(material!=observed.material || id!=observed.contributor ||
           (expected.value<0)!=( !observed.material.empty())) {
          std::cerr<<std::setprecision(17)<<"C05_MISMATCH scene="<<scene.id
            <<" point=("<<p.x<<","<<p.y<<","<<p.z<<")"
            <<" canonical_value="<<expected.value<<" canonical_material="<<material
            <<" canonical_contributor="<<id<<" actual_material="<<observed.material
            <<" actual_contributor="<<observed.contributor<<"\n";
          throw std::runtime_error("E_SEMANTIC_PARITY");
        }
        ++comparisons;
        if(boundary) ++boundaries;
      };
      // Finite 3-D survey, independent of scene-specific analytical branch.
      const std::array<double,15> grid{
        -1.8,-1.3,-1.0,-0.8,-0.65,-0.55,-0.35,0,0.35,0.55,0.65,0.8,1.0,1.3,1.8
      };
      for(double x:grid)for(double y:grid)for(double z:grid) check({x,y,z},false);
      // Explicit face/edge/vertex, sphere and CSG boundaries, plus nearest
      // representable points on both sides. No epsilon in occupancy.
      for(const auto& body:scene.bodies) {
        if(body.kind==rvi::Kind::sphere) {
          check(body.center,false);
          for(int axis=0;axis<3;++axis)for(double sign:{-1.0,1.0}) {
            Vec p=body.center;
            double* coordinate=axis==0?&p.x:(axis==1?&p.y:&p.z);
            *coordinate+=sign*body.radius;
            check(p,true);
            Vec inside=p,outside=p;
            double* i=axis==0?&inside.x:(axis==1?&inside.y:&inside.z);
            double* o=axis==0?&outside.x:(axis==1?&outside.y:&outside.z);
            *i=std::nextafter(*i,body.center.x*(axis==0)+
                 body.center.y*(axis==1)+body.center.z*(axis==2));
            *o=std::nextafter(*o,sign>0?INFINITY:-INFINITY);
            check(inside,true);check(outside,true);
          }
        } else {
          const auto box_checks=[&](Vec lo,Vec hi) {
            const Vec c=mid(lo,hi);
            check(c,false);
            for(int axis=0;axis<3;++axis)for(int side=0;side<2;++side) {
              Vec p=c;
              if(axis==0)p.x=side?hi.x:lo.x;
              else if(axis==1)p.y=side?hi.y:lo.y;
              else p.z=side?hi.z:lo.z;
              check(p,true);
            }
            for(int x=0;x<2;++x)for(int y=0;y<2;++y)for(int z=0;z<2;++z)
              check({x?hi.x:lo.x,y?hi.y:lo.y,z?hi.z:lo.z},true);
          };
          box_checks(body.lower,body.upper);
          if(body.kind==rvi::Kind::panel)box_checks(body.hole_lower,body.hole_upper);
        }
      }
    }
    if(comparisons<33000||boundaries<100)throw std::runtime_error("E_WITNESS_COVERAGE");
    std::cout<<"{\"schema\":\"fgm-v02-c05-witness-1\",\"ok\":true,"
             <<"\"core_pin\":\"ba91a5e96d9f4f9580c9b2e9a9460b97acb7f01b\","
             <<"\"scenes\":"<<scenes.size()<<",\"probes\":"<<comparisons
             <<",\"boundary_probes\":"<<boundaries<<"}\n";
  }catch(const std::exception& e) {
    std::cerr<<"V02 C05 witness FAIL: "<<e.what()<<"\n";
    return 1;
  }
}
