#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../src/nativeimage.h"
int main() {
    alignas(16) uint8_t image[4096]{};
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image); dos->e_lfanew = 0x80;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + 0x80);
    nt->OptionalHeader.SizeOfImage = sizeof(image);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = {0x200, 3 * sizeof(RUNTIME_FUNCTION)};
    auto table = reinterpret_cast<RUNTIME_FUNCTION*>(image + 0x200);
    table[0] = {0x500,0x541,0x300}; table[1] = {0x541,0x660,0x320}; table[2] = {0x700,0x780,0x350};
    image[0x300] = 1; image[0x350] = 1;
    image[0x320] = 1 | (UNW_FLAG_CHAININFO << 3); image[0x322] = 3;
    memcpy(image + 0x32c, &table[0], sizeof(RUNTIME_FUNCTION));
    uintptr_t begin{},end{}; uintptr_t base = reinterpret_cast<uintptr_t>(image);
    bool ok = nativehighlight::detail::FunctionRange(image,image+0x500,begin,end) && begin==base+0x500 && end==base+0x660;
    printf("split function includes chained callback return: %s\n",ok?"OK":"FAIL");
    bool ordinary = nativehighlight::detail::FunctionRange(image,image+0x700,begin,end) && begin==base+0x700 && end==base+0x780;
    printf("ordinary function excludes unrelated regions: %s\n",ordinary?"OK":"FAIL"); ok &= ordinary;
    auto chain = reinterpret_cast<RUNTIME_FUNCTION*>(image + 0x32c); chain->UnwindData=0x320;
    bool cycle = !nativehighlight::detail::FunctionRange(image,image+0x500,begin,end);
    printf("malformed unwind cycle fails closed: %s\n",cycle?"OK":"FAIL"); ok &= cycle;
    chain->UnwindData=0x2000;
    bool bounds = !nativehighlight::detail::FunctionRange(image,image+0x500,begin,end);
    printf("out of image unwind pointer fails closed: %s\n",bounds?"OK":"FAIL"); ok &= bounds;
    return ok?0:1;
}
