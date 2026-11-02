#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace dialog_input {
// Native TextDialog's field id is a build-dependent UI hash, not a stable ABI id.
inline constexpr char fieldIdPattern[]=
    "49 BD ?? ?? ?? ?? ?? ?? ?? ?? 66 44 89 A5 08 04 00 00 48 8B 88 C8 00 00 00 B8 02 00 00 00";
inline bool FieldId(const uint8_t* code,size_t size,uint64_t& id){
    constexpr uint8_t tail[]{0x66,0x44,0x89,0xa5,0x08,0x04,0,0,0x48,0x8b,0x88,0xc8,0,0,0,0xb8,2,0,0,0};
    if(size<30||code[0]!=0x49||code[1]!=0xbd||std::memcmp(code+10,tail,sizeof(tail)))return false;
    uint64_t value{};std::memcpy(&value,code+2,8);if(!value)return false;id=value;return true;
}
struct Session {
    int source=-1,expected=-1;void* world=nullptr;uint64_t opened=0;bool reached=false;
    void Begin(int from,int target,void* currentWorld,uint64_t now){*this={from,target,currentWorld,now,false};}
    bool Retire(int screen,void* currentWorld,uint64_t now){
        if(currentWorld!=world||!currentWorld)return true;
        if(screen==expected){reached=true;return false;}
        // Allow only the asynchronous transition Find requested, not unrelated
        // screens opened by a teleport, map, or another native menu.
        return reached||screen!=source||now<opened||now-opened>500;
    }
};
}
