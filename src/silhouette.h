#pragma once
#include "proxy_math.h"
namespace silhouette {
struct Snapshot { unsigned char asset[0x160],mesh[56]; nativehighlight::detail::Spatial transform; float alpha; };
void Draw(const Snapshot* snapshots,unsigned count);
}
