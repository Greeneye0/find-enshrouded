#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include "silhouette.h"
#include "silhouette_mesh.h"
#include "silhouette_raster.h"
#include "silhouette_runs.h"
#include "ui.h"
#include "projection_frame.h"
#include "log.h"
namespace silhouette {
namespace {
std::filesystem::path GameDir(){wchar_t p[32768];GetModuleFileNameW(nullptr,p,32768);return std::filesystem::path(p).parent_path();}
bool ReadFile(const std::filesystem::path& p,uint64_t at,uint32_t n,std::vector<uint8_t>& out){
    std::ifstream f(p,std::ios::binary);if(!f)return false;
    f.seekg(0,std::ios::end);auto size=f.tellg();if(size<0||at>uint64_t(size)||n>uint64_t(size)-at)return false;
    out.resize(n);f.seekg(at);f.read(reinterpret_cast<char*>(out.data()),n);return bool(f);
}
std::map<std::string,Mesh> cache;
bool Content(const uint8_t* hash,std::vector<uint8_t>& data){
    static std::vector<uint8_t> directory;
    static bool attempted=false;
    if(!attempted){attempted=true;auto p=GameDir()/"enshrouded.kfc";std::error_code ec;auto n=std::filesystem::file_size(p,ec);if(!ec&&n>=0x60&&n<64*1024*1024)ReadFile(p,0,static_cast<uint32_t>(n),directory);}
    if(directory.size()<0x60||memcmp(directory.data(),"KFC4",4))return false;
    const auto* d=directory.data();uint64_t keys=0x48ull+Read<uint32_t>(d+0x48),values=0x50ull+Read<uint32_t>(d+0x50);uint32_t count=Read<uint32_t>(d+0x4c),n=Read<uint32_t>(hash);
    if(count!=Read<uint32_t>(d+0x54)||keys+uint64_t(count)*16>directory.size()||values+uint64_t(count)*16>directory.size()||n>16*1024*1024)return false;
    for(uint32_t i=0;i<count;++i)if(!memcmp(d+keys+uint64_t(i)*16,hash,16)){
        uint64_t entry=Read<uint64_t>(d+values+uint64_t(i)*16);
        if(Read<uint64_t>(d+values+uint64_t(i)*16+8))return false;
        wchar_t filename[64];swprintf_s(filename,L"enshrouded_%03u.dat",unsigned(entry>>48));
        return ReadFile(GameDir()/filename,entry&0xffffffffffffull,n,data);
    }
    return false;
}
struct Face { std::array<float,3> normal{}; unsigned char shade=0; bool valid=false; };
struct Prepared {
    const Mesh* mesh=nullptr;nativehighlight::detail::Spatial transform{};
    ui::ProjectionFrame camera{};bool imageValid=false;
    std::vector<std::array<float,3>> worlds,points;
    std::vector<Face> faces;
    std::array<std::vector<ui::SolidRect>,7> strips;
};
Prepared g_prepared[256];
struct Scratch {
    std::vector<uint8_t> mask,horizontal,expanded,labels;
    std::vector<SurfacePixel> surface;
    std::vector<RasterRect> rectangles;
};
Scratch g_scratch;
void Submit(const Snapshot& s,const Prepared& prepared) {
    const auto& strips=prepared.strips;
    float pulse=.5f-.5f*std::cos(float(GetTickCount64()%1800)/1800.f*6.283185307f);
    for(int k=1;k<=4;++k)ui::DrawSolids(strips[k].data(),strips[k].size(),0xff6ecdff,s.alpha*(.08f+k*.025f+(.08f+k*.025f)*pulse));
    ui::DrawSolids(strips[6].data(),strips[6].size(),0xff6ecdff,s.alpha*(.08f+.10f*pulse));
    ui::DrawSolids(strips[5].data(),strips[5].size(),0xff6ecdff,s.alpha*(.45f+.40f*pulse));
}
void DrawOne(const Snapshot& s,const Mesh& mesh,const ui::ProjectionFrame& camera,Prepared& prepared){
    if(mesh.vertices.empty())return;
    bool sameGeometry=prepared.mesh==&mesh && !memcmp(&prepared.transform,&s.transform,sizeof(s.transform));
    if(sameGeometry && prepared.imageValid && !memcmp(&prepared.camera,&camera,sizeof(camera))) { Submit(s,prepared);return; }
    prepared.imageValid=false;
    auto& points=prepared.points;auto& worlds=prepared.worlds;auto& faces=prepared.faces;
    points.clear();points.reserve(mesh.vertices.size());
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
    const auto& t=s.transform;
    double qnorm=0;for(float q:t.rotation){if(!std::isfinite(q))return;qnorm+=q*q;}
    if(std::fabs(qnorm-1)>0.02)return;
    for(float scale:t.scale)if(!std::isfinite(scale)||std::fabs(scale)>20)return;
    if(!sameGeometry){
        worlds.clear();worlds.reserve(mesh.vertices.size());
        float qx=t.rotation[0],qy=t.rotation[1],qz=t.rotation[2],qw=t.rotation[3];
        for(auto p:mesh.vertices){
            for(int j=0;j<3;++j)p[j]*=t.scale[j];
            float cx=2*(qy*p[2]-qz*p[1]),cy=2*(qz*p[0]-qx*p[2]),cz=2*(qx*p[1]-qy*p[0]);
            worlds.push_back({float(t.position[0]/4294967296.0)+p[0]+qw*cx+qy*cz-qz*cy,float(t.position[1]/4294967296.0)+p[1]+qw*cy+qz*cx-qx*cz,float(t.position[2]/4294967296.0)+p[2]+qw*cz+qx*cy-qy*cx});
        }
        faces.assign(mesh.indices.size()/3,{});
        for(size_t i=0;i<mesh.indices.size();i+=3){
            auto p=worlds[mesh.indices[i]],q=worlds[mesh.indices[i+1]],r=worlds[mesh.indices[i+2]];
            std::array<float,3> a{},b{};for(int j=0;j<3;++j){a[j]=q[j]-p[j];b[j]=r[j]-p[j];}
            auto& f=faces[i/3];f.normal={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
            auto& n=f.normal;float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);if(length<1e-8f)continue;
            for(float& v:n)v/=length;
            float light=std::fabs(n[0]*.36f+n[1]*.80f+n[2]*-.48f);
            f.shade=static_cast<unsigned char>(std::min(3,int(light*4)));f.valid=true;
        }
        prepared.mesh=&mesh;prepared.transform=s.transform;
    }
    for(const auto& world:worlds){
        float x,y,z;if(!camera.Project(world.data(),x,y,z)||std::fabs(x)>20000||std::fabs(y)>20000)return;
        points.push_back({x,y,1.f/z});minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y);
    }
    // Entirely offscreen images cannot contribute. Keep a margin for the halo.
    if(maxx<camera.viewport[0]-10 || maxy<camera.viewport[1]-10 || minx>camera.viewport[0]+camera.viewport[2]+10 || miny>camera.viewport[1]+camera.viewport[3]+10)return;
    float step=std::max(1.f,std::max(maxx-minx,maxy-miny)/250.f);
    if(maxx-minx<1||maxy-miny<1||step>20)return;
    int w=int(std::ceil((maxx-minx)/step))+7,h=int(std::ceil((maxy-miny)/step))+7;
    minx-=3*step;miny-=3*step;
    auto& mask=g_scratch.mask;mask.assign(w*h,0);
    auto& surface=g_scratch.surface;surface.assign(w*h,{});
    for(size_t i=0;i<mesh.indices.size();i+=3){
        const auto& face=faces[i/3];if(!face.valid)continue;
        std::array<float,3> p[3];
        for(int j=0;j<3;++j){auto v=points[mesh.indices[i+j]];p[j]={(v[0]-minx)/step,(v[1]-miny)/step,v[2]};}
        RasterSurface(surface,w,h,p,face.normal,face.shade);
    }
    for(int i=0;i<w*h;++i)mask[i]=surface[i].inverseDepth>0?1:0;
    // Two exterior contour bands soften the rasterized outline without obscuring the scene.
    // Separable 5x5 dilation: ten neighbour checks rather than twenty-five.
    auto& horizontal=g_scratch.horizontal;auto& expanded=g_scratch.expanded;auto& labels=g_scratch.labels;
    horizontal.assign(w*h,0);expanded.assign(w*h,0);labels.assign(w*h,0);
    for(int y=0;y<h;++y)for(int x=2;x<w-2;++x)
        horizontal[y*w+x]=mask[y*w+x-2]|mask[y*w+x-1]|mask[y*w+x]|mask[y*w+x+1]|mask[y*w+x+2];
    for(int y=2;y<h-2;++y)for(int x=2;x<w-2;++x)
        expanded[y*w+x]=horizontal[(y-2)*w+x]|horizontal[(y-1)*w+x]|horizontal[y*w+x]|horizontal[(y+1)*w+x]|horizontal[(y+2)*w+x];
    for(int y=1;y<h-1;++y)for(int x=1;x<w-1;++x){
        int at=y*w+x;
        if(!mask[at]){labels[at]=expanded[at]?6:0;continue;}
        if(!mask[at-1]||!mask[at+1]||!mask[at-w]||!mask[at+w]||
            SurfaceCrease(surface[at],surface[at+1])||SurfaceCrease(surface[at],surface[at+w]))labels[at]=5;
        else labels[at]=1+surface[at].shade;
    }
    auto& rectangles=g_scratch.rectangles;
    MergeRuns(labels,w,h,rectangles);
    auto& strips=prepared.strips;for(auto& v:strips)v.clear();
    for(const auto& r:rectangles)strips[r.kind].push_back({minx+r.x*step,miny+r.y*step,r.w*step,r.h*step});
    prepared.camera=camera;prepared.imageValid=true;
    Submit(s,prepared);

}
}
void Draw(const Snapshot* snapshots,unsigned count){
    ui::ProjectionFrame camera;if(!ui::CaptureProjection(camera))return;
    count=std::min(count,256u);
    for(unsigned i=0;i<count;++i){const auto& s=snapshots[i];std::string key(reinterpret_cast<const char*>(s.asset+0x110),16);key.append(reinterpret_cast<const char*>(s.mesh),56);
        auto it=cache.find(key);if(it==cache.end()){
            if(cache.size()>=64)continue;
            Mesh mesh;std::vector<uint8_t> content;bool ok=Content(s.asset+0x110,content)&&Decode(content,s.asset,s.mesh,mesh);
            Log("silhouette: mesh decoded=%u vertices=%zu triangles=%zu",ok?1:0,mesh.vertices.size(),mesh.indices.size()/3);it=cache.emplace(key,std::move(mesh)).first;
        }
        DrawOne(s,it->second,camera,g_prepared[i]);
    }
}
}

