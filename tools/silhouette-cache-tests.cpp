#include "../src/silhouette.cpp"
#include <cassert>
#include <iostream>
#include <chrono>
#include <random>
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
 // A dense, bumpy sphere fragments like the 8.5k-triangle hazelnut bush that exhausted the
 // game's UI buffer. Its rectangles must fit the chest budget, and a frame budget must hold.
 {
  silhouette::Mesh dense;const int rings=60,segments=72;std::mt19937 random(99);
  for(int r=0;r<=rings;++r)for(int g=0;g<segments;++g){
   float th=3.14159265f*r/rings,ph=6.2831853f*g/segments,radius=1.6f+float(random()%1000)/4000.f;
   dense.vertices.push_back({{radius*std::sin(th)*std::cos(ph),radius*std::sin(th)*std::sin(ph),radius*std::cos(th)}});
  }
  for(int r=0;r<rings;++r)for(int g=0;g<segments;++g){
   uint16_t a=uint16_t(r*segments+g),b=uint16_t(r*segments+(g+1)%segments),c=uint16_t(a+segments),d=uint16_t(b+segments);
   dense.indices.insert(dense.indices.end(),{a,c,b,b,c,d});
  }
  silhouette::Prepared densePrepared;emitted.clear();silhouette::DrawOne(snapshot,dense,camera,densePrepared);
  size_t chest=emitted.size();
  std::cout<<"Dense mesh: "<<dense.indices.size()/3<<" triangles, "<<chest<<" rectangles\n";
  assert(chest>0&&chest<=silhouette::kChestRects);
  size_t budget=chest-1;emitted.clear();silhouette::DrawOne(snapshot,dense,camera,densePrepared,&budget);
  assert(emitted.empty()&&budget==chest-1);
  budget=chest+5;silhouette::DrawOne(snapshot,dense,camera,densePrepared,&budget);
  assert(emitted.size()==chest&&budget==5);
  // Twelve chests share the frame: each one coarsens to its share.
  silhouette::Prepared shared;budget=silhouette::kFrameRects;emitted.clear();
  silhouette::DrawOne(snapshot,dense,camera,shared,&budget,silhouette::kFrameRects/12);
  std::cout<<"Dense mesh at a twelfth of the frame: "<<emitted.size()<<" rectangles\n";
  assert(!emitted.empty()&&emitted.size()<=silhouette::kFrameRects/12);
 }
 auto bench=[&](bool invalidate){auto start=std::chrono::steady_clock::now();for(int i=0;i<1000;++i){if(invalidate)prepared.imageValid=false;draw();}return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/1000;};
 std::cout<<"Cache coverage, camera/viewport/transform invalidation and offscreen return passed.\n";
 std::cout<<"Offline cube CPU generation (mock UI): rebuilding "<<bench(true)<<" ms, exact-view reuse "<<bench(false)<<" ms. Not game frame timing.\n";
}
