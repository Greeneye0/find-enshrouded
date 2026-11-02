#include "../src/dialog_input.h"
#include "../src/nativeimage.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
#include <cstdlib>
int main(int argc,char** argv){
    using namespace dialog_input;
    uint8_t code[]{0x49,0xbd,0,0,0,0,0,0,0,0,0x66,0x44,0x89,0xa5,8,4,0,0,0x48,0x8b,0x88,0xc8,0,0,0,0xb8,2,0,0,0};
    for(uint64_t id:{0x64d2790860d9c7f6ull,0x397c0808484b9ef3ull}){
        std::memcpy(code+2,&id,8);uint64_t decoded=0;assert(FieldId(code,sizeof(code),decoded)&&decoded==id);
    }
    uint64_t decoded=0;assert(!FieldId(code,29,decoded));code[10]^=1;assert(!FieldId(code,30,decoded));code[10]^=1;
    Session session;int world=0,other=0;
    session.Begin(1,24,&world,1000);
    assert(!session.Retire(1,&world,1001));assert(!session.Retire(24,&world,1010));
    assert(session.Retire(5,&world,1011)); // Map/vanilla teleport closes the hidden field immediately.
    session.Begin(9,9,&world,1000);assert(!session.Retire(9,&world,1001));
    assert(session.Retire(1,&world,1002)); // Return from a custom teleport while search began in inventory.
    assert(session.Retire(9,&other,1002));assert(session.Retire(9,nullptr,1002));
    session.Begin(1,24,&world,1000);assert(session.Retire(0,&world,1001));
    session.Begin(1,24,&world,1000);assert(session.Retire(1,&world,1501));
    if(argc>1){
        std::ifstream file(argv[1],std::ios::binary);assert(file);
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(file)),{});
        auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
        auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data()+dos->e_lfanew);
        std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage);
        std::memcpy(image.data(),raw.data(),nt->OptionalHeader.SizeOfHeaders);
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){auto& section=IMAGE_FIRST_SECTION(nt)[i];
            std::memcpy(image.data()+section.VirtualAddress,raw.data()+section.PointerToRawData,section.SizeOfRawData);
        }
        size_t hits=0;
        for(size_t at=0;at+30<image.size();++at){
            uint64_t id{};if(!FieldId(image.data()+at,30,id))continue;
            uintptr_t begin{},end{};assert(nativehighlight::detail::FunctionRange(image.data(),image.data()+0xcdd950,begin,end));
            assert(begin-reinterpret_cast<uintptr_t>(image.data())==0xcdd950);
            assert(reinterpret_cast<uintptr_t>(image.data()+at)>=begin&&reinterpret_cast<uintptr_t>(image.data()+at)<end);
            assert(id==0x397c0808484b9ef3ull);++hits;
        }
        assert(hits==1);puts("PASS: actual 1.0.1.0 field identifier uniquely belongs to the native TextDialog");
    }
    puts("PASS: build-dependent text-field identity, map/teleport/world transitions, opening grace and stable visible dialog");
}
