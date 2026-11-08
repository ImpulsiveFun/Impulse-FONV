// Test-harness runtime: owns the 4 GB guest space and runs one recompiled function with a trap
// guard, so difftest.py can drive it through ctypes.
#include <setjmp.h>
#include <sys/mman.h>
#ifdef __ANDROID__
#include <stdatomic.h>
#include <android/log.h>
#include <pthread.h>
#include <time.h>
#endif
#include "rt.h"

#ifndef RT_FIXED_MEM
uint8_t *MEM;
#endif
#ifdef __ANDROID__
#define THREAD_STATE _Thread_local
#else
#define THREAD_STATE
#endif
#ifndef __ANDROID__
int64_t rt_budget;
#endif
static THREAD_STATE jmp_buf trap_jb;
static THREAD_STATE int trap_code;
static THREAD_STATE uint32_t trap_addr;
#ifdef __ANDROID__
typedef struct {
    int is_worker;
    RtGuard *guard;
} TraceThreadState;
static _Thread_local TraceThreadState trace_thread;
static _Atomic int stopping, worker_fault, trace_cancel_requested;
void rt_request_trace_stop(void){atomic_store(&trace_cancel_requested,1);}
static _Atomic unsigned trace_timeout_ms=30000;
void rt_trace_timeout_ms(unsigned ms){atomic_store(&trace_timeout_ms,ms);}
int64_t rt_trace_branch_budget(void){
    unsigned ms = atomic_load(&trace_timeout_ms);
    return ms ? (int64_t)ms * 50000 : INT64_MAX;
}
static uint32_t worker_fault_address;
static pthread_mutex_t fault_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t now_ns(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}
void rt_worker_begin(void) { trace_thread.is_worker = 1; }
int rt_worker_current(void) { return trace_thread.is_worker; }
void rt_stack_report(CPU *c,const char *reason,uint32_t object) {
    if(!c)return;
    uint32_t fp=(uint32_t)LD(c->r+20,4),sp=(uint32_t)LD(c->r+16,4);
    uint32_t pc[8]={0};unsigned count=0;
    // Best-effort x86 frame-pointer chain; optimized frames may be absent.
    while(count<8&&fp>=sp&&fp<0xfffffff8u&&fp-sp<16u*1024*1024){
        pc[count++]=(uint32_t)LD(MEM+fp+4,4);
        uint32_t next=(uint32_t)LD(MEM+fp,4);if(next<=fp)break;fp=next;
    }
    __android_log_print(ANDROID_LOG_INFO,"FNVStack","%s worker=%d object=%#x esp=%#x ebp=%#x ecx=%#x frames=%u %#x %#x %#x %#x %#x %#x %#x %#x",reason,trace_thread.is_worker,object,sp,(uint32_t)LD(c->r+20,4),(uint32_t)LD(c->r+4,4),count,pc[0],pc[1],pc[2],pc[3],pc[4],pc[5],pc[6],pc[7]);
    if(!trace_thread.is_worker){
        uint32_t pending=(uint32_t)LD(MEM+0x11d7324,4),finished=(uint32_t)LD(MEM+0x11d7334,4);
        if(pending||finished){
            uint32_t array=(uint32_t)LD(MEM+0x11d7320,4),task=0,vt=0;
            if(pending&&pending<65536&&array>=0x10000&&array<0xfffffffcu){
                task=(uint32_t)LD(MEM+array,4);
                if(task>=0x10000&&task<0xfffffffcu)vt=(uint32_t)LD(MEM+task,4);
            }
            __android_log_print(ANDROID_LOG_INFO,"FNVQueue","pending=%u finished=%u array=%#x first=%#x vtable=%#x",pending,finished,array,task,vt);
        }
    }
}
void rt_workers_stop(void) { atomic_store(&stopping, 1); }
int rt_workers_stopping(void) { return atomic_load(&stopping); }
void rt_workers_reset(void) { atomic_store(&worker_fault, 0); atomic_store(&stopping, 0); atomic_store(&trace_cancel_requested,0); }
void rt_worker_failure(int code, uint32_t address) {
    pthread_mutex_lock(&fault_lock);
    if (!atomic_load(&worker_fault)) {
        worker_fault_address = address;
        atomic_store(&worker_fault, code);
    }
    pthread_mutex_unlock(&fault_lock);
    rt_workers_stop();
}
void rt_poll(CPU *c, uint32_t address) {
    // Host wait shims can poll without passing the active guest CPU.
    rt_check_guard(c, address, trace_thread.guard);
}

void rt_poll_slow(CPU *c, uint32_t address, RtGuard *guard) {
    extern void host_graphics_poll(void) __attribute__((weak));
    if(host_graphics_poll)host_graphics_poll();
    TraceThreadState *thread = &trace_thread;
    unsigned polls = guard->polls;
    // Each branch still spends its full budget. Check shared stop/fault state
    // within 32 polls and keep the original 256-poll clock cadence.
    if (!(polls & 31)) {
        if (thread->is_worker && atomic_load(&stopping)) rt_trap(c, TRAP_CANCELLED, address);
        int fault = atomic_load(&worker_fault);
        if (!thread->is_worker && fault) rt_trap(c, fault, worker_fault_address);
        if (atomic_load(&trace_cancel_requested)) rt_trap(c, TRAP_CANCELLED, address);
        if (!(polls & 255) && guard->deadline_ns != UINT64_MAX &&
            now_ns() >= guard->deadline_ns) rt_trap(c, TRAP_BUDGET, address);
    }
    if(address==0xb01f50u&&c){
        uint32_t sp=(uint32_t)LD(c->r+16,4);
        __android_log_print(ANDROID_LOG_INFO,"FNVQueue","pool shutdown caller=%#x pool=%#x",sp>=0x10000&&sp<0xfffffffcu?(uint32_t)LD(MEM+sp,4):0,(uint32_t)LD(c->r+4,4));
        rt_stack_report(c,"pool-shutdown",(uint32_t)LD(c->r+4,4));
    }
    if(address==0xb02420u&&c){
        uint32_t pool=(uint32_t)LD(c->r+4,4);
        if(pool>=0x10000&&pool<0xfffffd00u)
            __android_log_print(ANDROID_LOG_INFO,"FNVQueue","resume pool=%#x count=%u handles=%#x,%#x,%#x,%#x,%#x,%#x gate=%#x",pool,(uint32_t)LD(MEM+pool+0x1c,4),(uint32_t)LD(MEM+pool+0x20,4),(uint32_t)LD(MEM+pool+0x24,4),(uint32_t)LD(MEM+pool+0x28,4),(uint32_t)LD(MEM+pool+0x2c,4),(uint32_t)LD(MEM+pool+0x30,4),(uint32_t)LD(MEM+pool+0x34,4),(uint32_t)LD(MEM+pool+0xc,4));
    }
#ifdef RT_PICK_DIAGNOSTICS
    if(!thread->is_worker&&c&&(address==0x7fe6c0u||address==0x7fb790u)){
        static _Thread_local unsigned menu_samples[2];
        unsigned slot=address==0x7fe6c0u?0:1,sample=++menu_samples[slot];
        if(sample<=4||!(sample&255)){
            uint32_t sp=(uint32_t)LD(c->r+16,4),self=(uint32_t)LD(c->r+4,4);
            if(sp>=0x10000u&&sp<0xfffffff0u&&self>=0x10000u&&self<0xfffffe00u)
                __android_log_print(ANDROID_LOG_INFO,"FNVPick","menu at=%#x caller=%#x self=%#x root=%#x camera=%#x arg=%#x tabs=%#x,%#x,%#x,%#x sample=%u",address,(uint32_t)LD(MEM+sp,4),self,(uint32_t)LD(MEM+self+4,4),(uint32_t)LD(MEM+self+0x18,4),(uint32_t)LD(MEM+sp+4,4),(uint32_t)LD(MEM+self+0xd8,4),(uint32_t)LD(MEM+self+0xdc,4),(uint32_t)LD(MEM+self+0xe0,4),(uint32_t)LD(MEM+self+0xe4,4),sample);
        }
    }
    // Observation-only samples of the pinned executable's 3D menu ray path.
    // Sample infrequently; do not alter guest registers, memory or pick results.
    if(!thread->is_worker&&c&&(address==0xa71080u||address==0xa703a0u||address==0xe98e20u)){
        static _Thread_local unsigned samples[9];
        unsigned slot=address==0xa71080u?0:address==0xa703a0u?1:2;
        uint32_t probe_sp=(uint32_t)LD(c->r+16,4);
        uint32_t probe_ret=probe_sp>=0x10000u&&probe_sp<0xfffffffcu?(uint32_t)LD(MEM+probe_sp,4):0;
        uint32_t probe_self=(uint32_t)LD(c->r+4,4);
        unsigned category=(probe_ret==0x7fb8afu||probe_ret==0x7fb8c1u)?1:(probe_ret==0x7fe81cu||probe_ret==0x7fe82eu)?2:(slot==1&&probe_self!=0x20004120u)?1:0;
        unsigned sample=++samples[slot+3*category];
        if(sample<=4||!(sample&1023)){
            uint32_t sp=(uint32_t)LD(c->r+16,4),self=(uint32_t)LD(c->r+4,4);
            if(sp>=0x10000u&&sp<0xffffffd0u&&self>=0x10000u&&self<0xfffffe00u){
                uint32_t ret=(uint32_t)LD(MEM+sp,4);
                if(slot==0){
                    float bounds[4];memcpy(bounds,MEM+self+0x100,16);
                    __android_log_print(ANDROID_LOG_INFO,"FNVPick","camera=%#x caller=%#x xy=%d,%d viewport=%g,%g,%g,%g sample=%u",self,ret,(int32_t)LD(MEM+sp+4,4),(int32_t)LD(MEM+sp+8,4),bounds[0],bounds[1],bounds[2],bounds[3],sample);
                }else if(slot==1){
                    float xy[2];memcpy(xy,MEM+sp+4,8);
                    __android_log_print(ANDROID_LOG_INFO,"FNVPick","ray camera=%#x caller=%#x normalized=%g,%g orthographic=%u sample=%u",self,ret,xy[0],xy[1],MEM[self+0xf4],sample);
                }else{
                    uint32_t pa=(uint32_t)LD(MEM+sp+4,4),pb=(uint32_t)LD(MEM+sp+8,4);
                    if(pa>=0x10000u&&pa<0xfffffff0u&&pb>=0x10000u&&pb<0xfffffff0u){
                        float origin[3],direction[3];memcpy(origin,MEM+pa,12);memcpy(direction,MEM+pb,12);
                        __android_log_print(ANDROID_LOG_INFO,"FNVPick","pick caller=%#x root=%#x origin=%g,%g,%g direction=%g,%g,%g sample=%u",ret,(uint32_t)LD(MEM+self+0x14,4),origin[0],origin[1],origin[2],direction[0],direction[1],direction[2],sample);
                    }
                }
            }
        }
    }
#endif
    if (!(polls & 0xfffff)) {
        uint32_t sp=c?(uint32_t)LD(c->r+16,4):0,ret=0,arg=0;
        if(sp>=0x10000&&sp<0xfffffff0u){ret=(uint32_t)LD(MEM+sp,4);arg=(uint32_t)LD(MEM+sp+4,4);}
        __android_log_print(ANDROID_LOG_INFO,"FNVHotspot","worker=%d at=%#x caller=%#x arg=%#x budget=%lld",thread->is_worker,address,ret,arg,(long long)guard->budget);
        if(!(polls&0xffffff))rt_stack_report(c,"running",address);
    }
}
#endif

extern const uint32_t rt_fn_addr[];
extern const GuestFn rt_fn_ptr[];
extern const int rt_fn_count;

void rt_trap(CPU *c, int code, uint32_t addr) {
    (void)c;
    trap_code = code;
    trap_addr = addr;
#ifdef __ANDROID__
    extern void host_graphics_unwind(void) __attribute__((weak));
    if(host_graphics_unwind)host_graphics_unwind();
#endif
    longjmp(trap_jb, 1);
}

uint8_t *rt_mem(void) {
#ifdef RT_FIXED_MEM
    static uint8_t *m;
    if (!m) {
        void *p = mmap((void *)RT_MEM_BASE, (size_t)1 << 32, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        m = p == (void *)RT_MEM_BASE ? (uint8_t *)p : NULL;
        if (!m && p != MAP_FAILED) munmap(p, (size_t)1 << 32);
    }
    return m;
#else
    if (!MEM) {
        void *p = mmap(NULL, (size_t)1 << 32, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        MEM = p == MAP_FAILED ? NULL : (uint8_t *)p;
    }
    return MEM;
#endif
}
int rt_count(void) { return rt_fn_count; }
uint32_t rt_addr(int i) { return rt_fn_addr[i]; }
uint32_t rt_trap_addr(void) { return trap_addr; }

// Returns 0 on normal return, else the trap code.
int rt_run(int i, CPU *c, int64_t budget) {
#ifdef __ANDROID__
    return rt_run_callback(c, rt_fn_ptr[i], budget);
#else
    rt_budget = budget;
    if (setjmp(trap_jb)) return trap_code;
    rt_fn_ptr[i](c);
    return 0;
#endif
}

#ifdef __ANDROID__
static int guarded_run(CPU *c, GuestFn callback, uint32_t entry, int64_t budget) {
    TraceThreadState *thread = &trace_thread;
    unsigned ms = atomic_load(&trace_timeout_ms);
    RtGuard guard = {budget, ms ? now_ns() + (uint64_t)ms * 1000000ull : UINT64_MAX, 0};
    // Saved guest register images copy this pointer, so restoring registers
    // leaves the running trace's accumulated budget and poll count intact.
    c->guard = thread->guard = &guard;
    int trapped = setjmp(trap_jb);
    if (!trapped) {
        if (callback) callback(c); else rt_call(c, entry);
    }
    extern void host_graphics_yield(void) __attribute__((weak));
    if(host_graphics_yield)host_graphics_yield();
    c->guard = thread->guard = NULL;
    return trapped ? trap_code : 0;
}
int rt_run_callback(CPU *c, GuestFn callback, int64_t budget) { return guarded_run(c, callback, 0, budget); }
int rt_run_entry(CPU *c, uint32_t entry, int64_t budget) { return guarded_run(c, NULL, entry, budget); }
#endif
