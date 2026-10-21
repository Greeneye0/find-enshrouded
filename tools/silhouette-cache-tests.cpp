#include "../src/silhouette.cpp"
#include <cassert>
#include <iostream>
#include <chrono>
std::vector<ui::SolidRect> emitted;
void Log(const char*,...){}
namespace ui {
bool CaptureProjection(ProjectionFrame&){return false;}
void DrawSolids(const SolidRect* r,size_t count,uint32_t,float){emitted.insert(emitted.end(),r,r+count);}
}
int main(){
 silhouette::Mesh mesh;mesh.vertices={{{-1,-1,0}},{{1,-1,0}},{{1,1,0}},{{-1,1,0}},{{-1,-1,1}},{{1,-1,1}},{{1,1,1}},{{-1,1,1}}};
 mesh.indices={0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,3,2,6,3,6,7,0,3,7,0,7,4,1,5,6,1,6,2};
 silhouette::Snapshot snapshot{};snapshot.transform.position[2]=int64_t(5)<<32;snapshot.transform.rotation[3]=1;for(float& s:snapshot.transform.scale)s=1;snapshot.alpha=1;
 ui::ProjectionFrame camera{};for(int i=0;i<4;++i){camera.view[i*5]=1;camera.projection[i*5]=1;}camera.viewport[2]=800;camera.viewport[3]=600;
 float x,y,z;float origin[3]={0,0,5};assert(camera.Project(origin,x,y,z)&&x==400&&y==300&&z==5);
 silhouette::Prepared prepared;
 auto draw=[&](){emitted.clear();silhouette::DrawOne(snapshot,mesh,camera,prepared);};
 draw();auto original=emitted;assert(!original.empty());draw();assert(emitted.size()==original.size()&&!memcmp(emitted.data(),original.data(),original.size()*sizeof(ui::SolidRect)));
 camera.view[12]=.25f;draw();assert(prepared.camera.view[12]==.25f);assert(emitted.size()!=original.size()||memcmp(emitted.data(),original.data(),original.size()*sizeof(ui::SolidRect)));
 snapshot.transform.scale[0]=.5f;draw();assert(prepared.transform.scale[0]==.5f);
 camera.viewport[2]=1600;draw();assert(prepared.camera.viewport[2]==1600);
 camera.view[12]=10000;draw();assert(emitted.empty()&&!prepared.imageValid);
 camera.view[12]=0;draw();assert(!emitted.empty());
 auto bench=[&](bool invalidate){auto start=std::chrono::steady_clock::now();for(int i=0;i<1000;++i){if(invalidate)prepared.imageValid=false;draw();}return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/1000;};
 std::cout<<"Cache coverage, camera/viewport/transform invalidation and offscreen return passed.\n";
 std::cout<<"Offline cube CPU generation (mock UI): rebuilding "<<bench(true)<<" ms, exact-view reuse "<<bench(false)<<" ms. Not game frame timing.\n";
}
