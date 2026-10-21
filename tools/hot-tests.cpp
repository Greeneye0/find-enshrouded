#define DllMain HotLoaderDllMain
#include "../loader/main.cpp"
#undef DllMain
#include <thread>
#include <atomic>
#include <cassert>
#include <iostream>
bool LoadProxyTargets(){return true;}
uint8_t* targetCode; uint8_t* otherCode;
int versionUnderTest=1; bool failInit=false,newSite=false;
const HotHost* hosts[3]{};
using TestFn=int(*)(); TestFn originals[3]{};
int Detour1(){bool on=hosts[1]->enter(hosts[1]->token);int n=originals[1]();hosts[1]->leave();return n+(on?10:0);}
int Detour2(){bool on=hosts[2]->enter(hosts[2]->token);int n=originals[2]();hosts[2]->leave();return n+(on?20:0);}
bool Prepared(){return true;}
bool Search(const char*){return true;}bool RuntimeStatus(void*){return true;}
extern "C" __declspec(dllexport) bool FindRuntimeInit(const HotHost* host,HotRuntime* api){
 hosts[versionUnderTest]=host;
 auto hook=host->hook(newSite?otherCode:targetCode,reinterpret_cast<void*>(versionUnderTest==1?Detour1:Detour2),12,targetCode,12);
 originals[versionUnderTest]=reinterpret_cast<TestFn>(hook);
 *api={sizeof(HotRuntime),1,Prepared,Prepared,[](){},Search,RuntimeStatus};
 return hook && !failInit;
}
Generation MakeGeneration(){Generation g;g.module=GetModuleHandleW(nullptr);return g;}
void Setup(Generation& g){g.host={sizeof(HotHost),1,&g,Enter,Leave,Find,Read,Hook,Poll};}
int main(){
 GetTempPathW(MAX_PATH,folder);
 targetCode=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
 otherCode=targetCode+128;memset(targetCode,0x90,12);targetCode[12]=0xb8;targetCode[13]=7;memset(targetCode+14,0,3);targetCode[17]=0xc3;memcpy(otherCode,targetCode,18);
 auto call=reinterpret_cast<TestFn>(targetCode);
 assert(call()==7);
 auto a=MakeGeneration();Setup(a);assert(Activate(&a));assert(call()==17);initial=false;
 uint8_t patched[18];memcpy(patched,targetCode,18);
 uint8_t canonical[12];Read(targetCode,canonical,12);for(auto c:canonical)assert(c==0x90);
 auto b=MakeGeneration();Setup(b);versionUnderTest=2;
 // An in-flight callback lease must defer publication.
 Enter(&a);pending=&b;Poll();assert(active==&a);Leave();
 std::atomic<bool> leased=false,releaseLease=false;
 std::thread holder([&]{Enter(&a);leased=true;while(!releaseLease)std::this_thread::yield();Leave();});
 while(!leased)std::this_thread::yield();
 Poll();assert(active==&a);releaseLease=true;holder.join();
 Poll();assert(active==&b);assert(call()==27);
 assert(memcmp(patched,targetCode,18)==0);assert(slots.size()==1);
 // Stale entries remain callable and pass through, because generations stay mapped.
 assert(Detour1()==7);
 // Nested hook entry must not reacquire a nonrecursive SRW lock.
 Enter(&b);assert(Detour2()==27);Leave();
 auto rejected=MakeGeneration();Setup(rejected);failInit=true;assert(!Activate(&rejected));hosts[2]=&b.host;originals[2]=reinterpret_cast<TestFn>(slots[0]->base);assert(active==&b);assert(call()==27);failInit=false;
 newSite=true;assert(!Activate(&rejected));hosts[2]=&b.host;originals[2]=reinterpret_cast<TestFn>(slots[0]->base);assert(active==&b);assert(call()==27);assert(slots.size()==1);newSite=false;
 // Stress atomic dispatch changes against a real concurrent caller.
 std::atomic<bool> stop=false;std::atomic<unsigned> calls=0;
 std::thread worker([&]{while(!stop){int n=call();assert(n==7 || n==17 || n==27);++calls;}});
 while(calls.load()<100) std::this_thread::yield();
 for(unsigned i=0;i<500;++i){AcquireSRWLockExclusive(&gate);auto g=(i&1)?&a:&b;active=g;InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slots[0]->thunk+16),reinterpret_cast<void*>((i&1)?Detour1:Detour2));ReleaseSRWLockExclusive(&gate);}
 stop=true;worker.join();
 std::cout<<"Hotloader lifecycle tests passed; concurrent calls="<<calls<<"\n";
}
