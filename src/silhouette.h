#pragma once
#include "proxy_math.h"
namespace silhouette {
struct Snapshot { unsigned char asset[0x160],mesh[56]; nativehighlight::detail::Spatial transform; float alpha; };
void Draw(const Snapshot* snapshots,unsigned count);
// The last Draw: outlines drawn and the vertices of their HUD mesh.
void LastDraw(unsigned& outlines, unsigned& vertices);
}
