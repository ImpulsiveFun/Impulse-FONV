// Guest-address -> host-function dispatch, plus the platform pieces generated code calls.
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <os/lock.h>
#else
#include <pthread.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <time.h>
#endif
#include <stdlib.h>
#include "rt.h"

extern const uint32_t rt_fn_addr[];
extern const GuestFn rt_fn_ptr[];
extern const int rt_fn_count;

// Import thunks (0xFEE00000 + 4*i) belong to the host; the test library has no host, so its
// weak default just traps like any other unknown address.
__attribute__((weak)) void rt_import_call(CPU *c, uint32_t a) { rt_trap(c, TRAP_NOFN, a); }
// No native function for an address: the host interprets it (fallback.c); the test library traps.
__attribute__((weak)) void rt_fallback(CPU *c, uint32_t a) {
#ifdef __ANDROID__
    uint32_t sp=(uint32_t)LD(c->r+16,4);uint32_t ret=sp>=0x10000&&sp<0xffffffc0u?(uint32_t)LD(MEM+sp,4):0;
    __android_log_print(ANDROID_LOG_ERROR,"FNVCall","Missing target=%#x caller=%#x esp=%#x eax=%#x ecx=%#x edx=%#x ebx=%#x esi=%#x edi=%#x worker=%d",a,ret,sp,(uint32_t)LD(c->r,4),(uint32_t)LD(c->r+4,4),(uint32_t)LD(c->r+8,4),(uint32_t)LD(c->r+12,4),(uint32_t)LD(c->r+24,4),(uint32_t)LD(c->r+28,4),rt_worker_current());
    if(sp>=0x10000&&sp<0xffffffc0u)__android_log_print(ANDROID_LOG_ERROR,"FNVCall","Stack=%#x,%#x,%#x,%#x,%#x,%#x",(uint32_t)LD(MEM+sp+4,4),(uint32_t)LD(MEM+sp+8,4),(uint32_t)LD(MEM+sp+12,4),(uint32_t)LD(MEM+sp+16,4),(uint32_t)LD(MEM+sp+20,4),(uint32_t)LD(MEM+sp+24,4));
    // Diagnose the pinned game's unchecked shader-factory result without changing execution.
    if(a==0 && ret==0xb57ab6u){
        uint32_t property=(uint32_t)LD(c->r+12,4);
        uint32_t type=property>=0x10000u&&property<0xffffff00u?(uint32_t)LD(MEM+property+0x58,4):0xffffffffu;
        uint32_t renderer=(uint32_t)LD(MEM+0x11f9508u,4);
        uint32_t cached=type<=34?(uint32_t)LD(MEM+0x11f9548u+4*type,4):0;
        __android_log_print(ANDROID_LOG_ERROR,"FNVCall","Shader factory: property=%#x type=%u renderer=%#x cached=%#x variant=%u",property,type,renderer,cached,(unsigned)MEM[0x11f94a6u]);
    }
#endif
    rt_trap(c, TRAP_NOFN, a);
}

// O(1) dispatch: one 32-bit slot per byte of the code range holds (function index + 1).
#define DISP_LO 0x00400000u
#define DISP_HI 0x01000000u
static uint32_t *disp;
static void disp_build(void) {
    uint32_t *t = calloc(DISP_HI - DISP_LO, sizeof *t);
    for (int i = 0; i < rt_fn_count; i++) {
        uint32_t a = rt_fn_addr[i];
        if (a >= DISP_LO && a < DISP_HI) t[a - DISP_LO] = (uint32_t)i + 1;
    }
    if (!__sync_bool_compare_and_swap(&disp, NULL, t)) free(t);
}
static inline int fn_index(uint32_t a) {
    if (a >= DISP_LO && a < DISP_HI) {
        if (__builtin_expect(!disp, 0)) disp_build();
        return (int)disp[a - DISP_LO] - 1;
    }
    int lo = 0, hi = rt_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = rt_fn_addr[mid];
        if (v == a) return mid;
        if (v < a) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

void rt_call(CPU *c, uint32_t a) {
    if (a >= 0xFEE00000u && a < 0xFEE10000u) { rt_import_call(c, a); return; }
    int i = fn_index(a);
    if (i >= 0) { rt_fn_ptr[i](c); return; }
    rt_fallback(c, a);
}

int rt_has_fn(uint32_t a) { return fn_index(a) >= 0; }

#ifdef RT_CALLTRACE
_Thread_local uint32_t rt_ct_ring[256];
_Thread_local uint32_t rt_ct_pos;
#endif
#include <stdio.h>
void rt_calltrace_dump(void) {
#ifdef RT_CALLTRACE
    fprintf(stderr, "[calltrace] last function entries on this thread (oldest first):\n");
    for (uint32_t i = rt_ct_pos > 64 ? rt_ct_pos - 64 : 0; i < rt_ct_pos; i++) fprintf(stderr, " %#x", rt_ct_ring[i & 255]);
    fprintf(stderr, "\n");
#endif
}

uint64_t rt_rdtsc(void) {
#ifdef __APPLE__
    return mach_absolute_time();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
#endif
}

// Report a plain SSE2-capable x86 (what the game's 2010 feature checks look for).
void rt_cpuid(CPU *c) {
    uint32_t leaf = (uint32_t)LD(c->r + 0, 4), a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0: a = 1; b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; break;   // "GenuineIntel"
    case 1: a = 0x000006fb; b = 0x00040800; cc = 0x00000001; d = 0x078bfbff; break; // SSE..SSE3
    default: break;
    }
    ST(c->r + 0, a, 4); ST(c->r + 12, b, 4); ST(c->r + 4, cc, 4); ST(c->r + 8, d, 4);
}

// global lock for x86 LOCK-prefixed read-modify-write instructions
#ifdef __APPLE__
static os_unfair_lock lock_prefix = OS_UNFAIR_LOCK_INIT;
void rt_lock(void) { os_unfair_lock_lock(&lock_prefix); }
void rt_unlock(void) { os_unfair_lock_unlock(&lock_prefix); }
#else
static pthread_mutex_t lock_prefix = PTHREAD_MUTEX_INITIALIZER;
void rt_lock(void) { pthread_mutex_lock(&lock_prefix); }
void rt_unlock(void) { pthread_mutex_unlock(&lock_prefix); }
#endif

#ifdef RT_X87CHECK
#include <stdio.h>
void rt_x87_violation(uint32_t at, int depth, int allowed) {
    static uint32_t seen[4096]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == at) return;
    if (n < 4096) seen[n++] = at;
    fprintf(stderr, "[x87] %s at %#x with %d values on the stack (ABI allows %d)\n", allowed == 1 ? "RETURN" : "CALL", at, depth, allowed);
}
#endif
