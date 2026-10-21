#include "../src/silhouette_mesh.h"
#include "../src/silhouette_raster.h"
#include <cassert>
#include <iostream>
#include <random>
#include "../src/silhouette_runs.h"
using namespace silhouette;
void Put(unsigned char* p,uint32_t n){memcpy(p,&n,4);}
int main(){
 unsigned char a[0x160]{},m[56]{};float one=1;for(int j=0;j<3;++j)memcpy(a+0x40+4*j,&one,4);
 Put(m+0x14,3);Put(m+0x1c,3);m[0x34]=4;m[0x35]=24;m[0x37]=1;Put(a+0x130,24);Put(a+0x134,6);Put(a+0x144,24);
 std::vector<uint8_t> data(30);uint32_t packed[6]={0,0,0xfffff800,0,0x7ff,0xffc00000};memcpy(data.data(),packed,24);uint16_t idx[3]={0,1,2};memcpy(data.data()+24,idx,6);
 Mesh mesh;assert(Decode(data,a,m,mesh));assert(mesh.vertices[1][0]==1.f);assert(mesh.vertices[2][1]==1.f);assert(mesh.vertices[0][2]==0);
 data[28]=9;assert(!Decode(data,a,m,mesh));data[28]=2;data.resize(29);assert(!Decode(data,a,m,mesh));data.resize(30);Put(m+0x28,0xffffffff);assert(!Decode(data,a,m,mesh));
 std::vector<SurfacePixel> pixels(64);
 std::array<float,3> triangle[3]={{{0,0,.1f}},{{8,0,.1f}},{{0,8,.1f}}};
 RasterSurface(pixels,8,8,triangle,{0,0,1},1);assert(pixels[9].shade==1);
 for(auto& v:triangle)v[2]=.2f;
 RasterSurface(pixels,8,8,triangle,{0,1,0},3);assert(pixels[9].shade==3);
 for(auto& v:triangle)v[2]=.1f;
 RasterSurface(pixels,8,8,triangle,{0,0,1},1);assert(pixels[9].shade==3);
 assert(!SurfaceCrease(pixels[9],pixels[10]));
 SurfacePixel perpendicular{.2f,{1,0,0},1};assert(SurfaceCrease(pixels[9],perpendicular));
 std::mt19937 random(1234);
 for(int trial=0;trial<100;++trial){
  int w=31,h=29;std::vector<uint8_t> labels(w*h);for(auto& k:labels)k=static_cast<uint8_t>(random()%7);
  if(trial==0)std::fill(labels.begin(),labels.end(),3);
  std::vector<RasterRect> rects;auto runs=MergeRuns(labels,w,h,rects);assert(rects.size()<=runs);
  if(trial==0)assert(rects.size()==1&&runs==29);
  std::vector<uint8_t> rebuilt(w*h),covered(w*h);
  for(const auto& r:rects)for(int y=r.y;y<r.y+r.h;++y)for(int x=r.x;x<r.x+r.w;++x){assert(!covered[y*w+x]);covered[y*w+x]=1;rebuilt[y*w+x]=static_cast<uint8_t>(r.kind);}
  assert(rebuilt==labels);
 }
 // Compare scan-conversion to independent barycentric coverage/depth at pixel centres.
 for(int trial=0;trial<200;++trial){
  std::array<float,3> p[3];for(auto& v:p)v={float(random()%4000)/100.f-4,float(random()%4000)/100.f-4,.1f+float(random()%1000)/1000.f};
  float area=(p[1][1]-p[2][1])*(p[0][0]-p[2][0])+(p[2][0]-p[1][0])*(p[0][1]-p[2][1]);if(std::fabs(area)<.01f)continue;
  std::vector<SurfacePixel> image(32*32);RasterSurface(image,32,32,p,{0,0,1},1);
  for(int y=0;y<32;++y)for(int x=0;x<32;++x){
   float a=((p[1][1]-p[2][1])*(x+.5f-p[2][0])+(p[2][0]-p[1][0])*(y+.5f-p[2][1]))/area;
   float b=((p[2][1]-p[0][1])*(x+.5f-p[2][0])+(p[0][0]-p[2][0])*(y+.5f-p[2][1]))/area,c=1-a-b;
   if(std::min({std::fabs(a),std::fabs(b),std::fabs(c)})<.0001f)continue;
   bool inside=a>=0&&b>=0&&c>=0;assert((image[y*32+x].inverseDepth>0)==inside);
   if(inside)assert(std::fabs(image[y*32+x].inverseDepth-(a*p[0][2]+b*p[1][2]+c*p[2][2]))<.0001f);
  }
 }
 std::cout<<"Silhouette decoder bounds and packed coordinates pass\n";
}
