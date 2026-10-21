#include "../src/proxy_math.h"
#include <cassert>
#include <iostream>
using namespace nativehighlight::detail;
constexpr double unit=4294967296.0;
void SetPosition(Spatial& s,double x,double y,double z){s.position[0]=static_cast<int64_t>(x*unit);s.position[1]=static_cast<int64_t>(y*unit);s.position[2]=static_cast<int64_t>(z*unit);}
int main(){
 Spatial eye{};eye.rotation[3]=1;eye.scale[0]=eye.scale[1]=eye.scale[2]=1;SetPosition(eye,3700,840,1400);
 Spatial original=eye;SetPosition(original,3702,841,1410);original.scale[0]=2;original.scale[1]=0.8f;original.scale[2]=1.2f;
 Spatial proxy=original;float factor=0;assert(PerspectiveProxy(eye,proxy,1.2f,factor));
 // The complete mesh, not only its origin, must preserve perspective projection.
 for(double x:{-0.5,0.0,0.5})for(double y:{0.0,1.0})for(double z:{-0.5,0.5}){
  double local[3]={x,y,z},a[3],b[3];
  for(int j=0;j<3;++j){a[j]=(original.position[j]-eye.position[j])/unit+local[j]*original.scale[j];b[j]=(proxy.position[j]-eye.position[j])/unit+local[j]*proxy.scale[j];}
  assert(std::fabs(a[0]/a[2]-b[0]/b[2])<1e-6);assert(std::fabs(a[1]/a[2]-b[1]/b[2])<1e-6);assert(b[2]>0.10);
 }
 // Camera yaw of 90 degrees: looking along positive X rather than positive Z.
 eye.rotation[1]=std::sqrt(0.5f);eye.rotation[3]=std::sqrt(0.5f);SetPosition(original,3710,841,1402);proxy=original;
 assert(PerspectiveProxy(eye,proxy,1.2f,factor));
 double oldX=(original.position[0]-eye.position[0])/unit,oldZ=(original.position[2]-eye.position[2])/unit;
 double newX=(proxy.position[0]-eye.position[0])/unit,newZ=(proxy.position[2]-eye.position[2])/unit;
 assert(std::fabs(oldZ/oldX-newZ/newX)<1e-6);
 SetPosition(original,3690,840,1400);proxy=original;assert(!PerspectiveProxy(eye,proxy,1.2f,factor));
 SetPosition(original,3700.2,840,1400);proxy=original;assert(!PerspectiveProxy(eye,proxy,1.2f,factor));
 eye.rotation[1]=NAN;proxy=original;assert(!PerspectiveProxy(eye,proxy,1.2f,factor));
 std::cout<<"Proxy projection, near-plane, camera yaw, behind-eye and malformed camera tests passed\n";
}
