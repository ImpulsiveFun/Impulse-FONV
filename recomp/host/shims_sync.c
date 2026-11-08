// Handles, threads and synchronisation: events, semaphores, mutexes, critical sections, waits.
// Each guest thread is a host pthread running recompiled code on its own CPU + stack + TEB.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "shims.h"
#ifdef __ANDROID__
#include <android/log.h>
extern void rt_stack_report(CPU *c,const char *reason,uint32_t object);
#endif

// ---- handle table ----------------------------------------------------------------------------
#define MAXH 65536
static struct { int type; void *obj; } htab[MAXH];
static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;
uint32_t handle_new(int type, void *obj) {
    pthread_mutex_lock(&hlock);
    for (uint32_t i = 64; i < MAXH; i++)
        if (!htab[i].type) { htab[i].type = type; htab[i].obj = obj; pthread_mutex_unlock(&hlock); return i * 4; }
    pthread_mutex_unlock(&hlock);
    return 0;
}
void *handle_get(uint32_t h, int type) {
    uint32_t i = h / 4;
    pthread_mutex_lock(&hlock);
    void *object = i < MAXH && htab[i].type == type ? htab[i].obj : NULL;
    pthread_mutex_unlock(&hlock);
    return object;
}
int handle_type(uint32_t h) {
    uint32_t i = h / 4; pthread_mutex_lock(&hlock);
    int type = i < MAXH ? htab[i].type : 0;
    pthread_mutex_unlock(&hlock); return type;
}
void handle_close(uint32_t h) {
    uint32_t i = h / 4; pthread_mutex_lock(&hlock);
#ifdef __ANDROID__
    if(i<MAXH&&htab[i].type==H_THREAD)__android_log_print(ANDROID_LOG_INFO,"FNVThread","handle closed=%#x",h);
#endif
    if (i < MAXH) { htab[i].type = 0; htab[i].obj = NULL; }
    pthread_mutex_unlock(&hlock);
}

// ---- waitable objects ------------------------------------------------------------------------
typedef struct Obj {
    pthread_mutex_t m; pthread_cond_t cv;
    int manual, signaled;          // event
    int count, max;                // semaphore
    pthread_t owner; int recursion; int owned;  // mutex
    int done; uint32_t exit_code;  // thread
    int suspend;                   // thread: >0 while suspended (CREATE_SUSPENDED / SuspendThread)
#ifdef __ANDROID__
    struct Obj *allocated_next;
#endif
} Obj;
#ifdef __ANDROID__
static Obj *allocated_objects;
#endif

static Obj *obj_new(void) {
    Obj *o = calloc(1, sizeof *o);
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->cv, NULL);
#ifdef __ANDROID__
    pthread_mutex_lock(&hlock);
    o->allocated_next = allocated_objects; allocated_objects = o;
    pthread_mutex_unlock(&hlock);
#endif
    return o;
}

// is the object signaled for the calling thread? (called with o->m held); consumes if it can
static int try_acquire(int type, Obj *o) {
    switch (type) {
    case H_EVENT: if (o->signaled) { if (!o->manual) o->signaled = 0; return 1; } return 0;
    case H_SEMA: if (o->count > 0) { o->count--; return 1; } return 0;
    case H_MUTEX:
        if (!o->owned || pthread_equal(o->owner, pthread_self())) { o->owned = 1; o->owner = pthread_self(); o->recursion++; return 1; }
        return 0;
    case H_THREAD: return o->done;
    }
    return 1;
}

static void deadline(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

// 0 = WAIT_OBJECT_0, 0x102 = WAIT_TIMEOUT
static uint32_t wait_one_context(CPU *cpu, uint32_t h, uint32_t ms) {
    int t = handle_type(h);
#ifdef __ANDROID__
    if (t < H_EVENT || t > H_THREAD) { SETERR(6); return 0xffffffffu; }
#endif
    Obj *o = t ? handle_get(h, t) : NULL;
    if (!o) return 0xFFFFFFFFu;
    struct timespec ts;
#ifdef __ANDROID__
    uint32_t bounded = ms;
    deadline(&ts, bounded);
    uint64_t next_report=host_uptime_ns()+5000000000ull;
#else
    deadline(&ts, ms);
#endif
    pthread_mutex_lock(&o->m);
    while (!try_acquire(t, o)) {
#ifdef __ANDROID__
        if (ms == 0) { pthread_mutex_unlock(&o->m); return 0x102; }
        struct timespec slice; deadline(&slice, 10);
        pthread_cond_timedwait(&o->cv, &o->m, &slice);
        pthread_mutex_unlock(&o->m);
        rt_poll(cpu, cpu?G32(REG(cpu,R_ESP)):h);
        uint64_t report_now=host_uptime_ns();
        if(report_now>=next_report){rt_stack_report(cpu,"wait-one",h);next_report=report_now+5000000000ull;}
        if (rt_workers_stopping()) return 0xffffffffu;
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (bounded != 0xffffffffu && (now.tv_sec > ts.tv_sec ||
            (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))) {
            
            return 0x102;
        }
        pthread_mutex_lock(&o->m);
#else
        if (ms == 0) { pthread_mutex_unlock(&o->m); return 0x102; }
        int r = ms == 0xFFFFFFFFu ? pthread_cond_wait(&o->cv, &o->m) : pthread_cond_timedwait(&o->cv, &o->m, &ts);
        if (r == ETIMEDOUT) { pthread_mutex_unlock(&o->m); return 0x102; }
#endif
    }
    pthread_mutex_unlock(&o->m);
    return 0;
}
static uint32_t wait_one(uint32_t h,uint32_t ms){return wait_one_context(NULL,h,ms);}

SHIM(CreateEventA) {
    Obj *o = obj_new(); o->manual = (int)ARG(c, 1); o->signaled = (int)ARG(c, 2);
    RET(c, handle_new(H_EVENT, o), 4);
}
SHIM(SetEvent) {
    Obj *o = handle_get(ARG(c, 0), H_EVENT);
    if (o) { pthread_mutex_lock(&o->m); o->signaled = 1; pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->m); }
    RET(c, o != NULL, 1);
}
SHIM(ResetEvent) {
    Obj *o = handle_get(ARG(c, 0), H_EVENT);
    if (o) { pthread_mutex_lock(&o->m); o->signaled = 0; pthread_mutex_unlock(&o->m); }
    RET(c, o != NULL, 1);
}
SHIM(CreateSemaphoreA) {
    int initial = (int)ARG(c, 1), maximum = (int)ARG(c, 2);
    if (initial < 0 || maximum <= 0 || initial > maximum) {
        SETERR(87); RET(c, 0, 4); return;
    }
    Obj *o = obj_new(); o->count = initial; o->max = maximum;
    RET(c, handle_new(H_SEMA, o), 4);
}
SHIM(ReleaseSemaphore) {
    Obj *o = handle_get(ARG(c, 0), H_SEMA); uint32_t prev = ARG(c, 2);
    if (!o) { RET(c, 0, 3); return; }
    pthread_mutex_lock(&o->m);
    int release = (int)ARG(c, 1);
    if (release <= 0 || release > o->max - o->count) {
        pthread_mutex_unlock(&o->m);
        SETERR(release <= 0 ? 87 : 298); RET(c, 0, 3); return;
    }
    if (prev) P32(prev, (uint32_t)o->count);
    o->count += release;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    RET(c, 1, 3);
}
SHIM(CreateMutexA) {
    Obj *o = obj_new();
    if (ARG(c, 1)) { o->owned = 1; o->owner = pthread_self(); o->recursion = 1; }
    RET(c, handle_new(H_MUTEX, o), 3);
}
SHIM(ReleaseMutex) {
    Obj *o = handle_get(ARG(c, 0), H_MUTEX);
    if (!o) { SETERR(6); RET(c, 0, 1); return; }
    pthread_mutex_lock(&o->m);
    int owned = o->owned && pthread_equal(o->owner, pthread_self());
    if (owned && --o->recursion == 0) {
        o->owned = 0;
        pthread_cond_broadcast(&o->cv);
    }
    pthread_mutex_unlock(&o->m);
    if (!owned) SETERR(288);
    RET(c, owned, 1);
}

#ifdef __ANDROID__
static uint32_t wait_all_android(uint32_t count, uint32_t array) {
    if (!count || count > 64) { SETERR(87); return 0xffffffffu; }
    Obj *objects[64];
    int types[64];
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t handle = G32(array + 4 * i);
        types[i] = handle_type(handle);
        if (types[i] < H_EVENT || types[i] > H_THREAD) { SETERR(6); return 0xffffffffu; }
        objects[i] = handle_get(handle, types[i]);
        if (!objects[i]) { SETERR(6); return 0xffffffffu; }
        for (uint32_t j = 0; j < i; ++j)
            if (objects[j] == objects[i]) { SETERR(87); return 0xffffffffu; }
    }
    // A stable order prevents two wait-all callers locking overlapping sets in
    // opposite orders. Check the whole set before consuming any signals.
    Obj *locks[64];
    for (uint32_t i = 0; i < count; ++i) {
        locks[i] = objects[i];
        for (uint32_t j = i; j && (uintptr_t)locks[j] < (uintptr_t)locks[j - 1]; --j) {
            Obj *swap = locks[j]; locks[j] = locks[j - 1]; locks[j - 1] = swap;
        }
    }
    for (uint32_t i = 0; i < count; ++i) pthread_mutex_lock(&locks[i]->m);
    int ready = 1;
    for (uint32_t i = 0; i < count; ++i) {
        Obj *o = objects[i];
        ready &= (types[i] == H_EVENT && o->signaled) ||
                 (types[i] == H_SEMA && o->count > 0) ||
                 (types[i] == H_THREAD && o->done) ||
                 (types[i] == H_MUTEX && (!o->owned || pthread_equal(o->owner, pthread_self())));
    }
    if (ready)
        for (uint32_t i = 0; i < count; ++i) try_acquire(types[i], objects[i]);
    for (uint32_t i = count; i > 0; --i) pthread_mutex_unlock(&locks[i - 1]->m);
    return ready ? 0 : 0x102;
}
#endif
SHIM(WaitForSingleObject) { RET(c, wait_one_context(c, ARG(c, 0), ARG(c, 1)), 2); }
SHIM(WaitForMultipleObjects) {
    uint32_t n = ARG(c, 0), arr = ARG(c, 1), all = ARG(c, 2), ms = ARG(c, 3);
    if (!n || n > 64) { SETERR(87); RET(c, 0xffffffffu, 4); return; }
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
#ifdef __ANDROID__
    uint64_t next_report=host_uptime_ns()+5000000000ull;
#endif
    for (;;) {      // simple polling implementation: correct, not fast; revisit if it shows up in profiles
        if (all) {
#ifdef __ANDROID__
            uint32_t status = wait_all_android(n, arr);
            if (status != 0x102) { RET(c, status, 4); return; }
#else
            uint32_t ok = 1;
            for (uint32_t i = 0; i < n; i++) ok &= wait_one(G32(arr + 4 * i), 0) == 0;
            if (ok) { RET(c, 0, 4); return; }
#endif
        } else {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t status = wait_one(G32(arr + 4 * i), 0);
                if (status == 0xffffffffu) { SETERR(6); RET(c, status, 4); return; }
                if (status == 0) { RET(c, i, 4); return; }
            }
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t el = ((int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
                       now.tv_nsec - start.tv_nsec) / 1000000;
#ifdef __ANDROID__
        rt_poll(c, G32(REG(c, R_ESP)));
        uint64_t report_now=host_uptime_ns();
        if(report_now>=next_report){rt_stack_report(c,"wait-multiple",G32(arr));next_report=report_now+5000000000ull;}
        if (rt_workers_stopping()) { SETERR(6); RET(c, 0xffffffffu, 4); return; }
        
#endif
        if (ms != 0xFFFFFFFFu && el >= ms) { RET(c, 0x102, 4); return; }
        usleep(200);
    }
}

// ---- critical sections: guest address -> host recursive mutex ---------------------------------
#define CS_BUCKETS 4096
typedef struct CSNode { uint32_t addr; pthread_mutex_t m; struct CSNode *next; } CSNode;
static CSNode *cs_tab[CS_BUCKETS];
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_get(uint32_t a) {
    uint32_t b = (a >> 3) % CS_BUCKETS;
    pthread_mutex_lock(&cs_lock);
    CSNode *n = cs_tab[b];
    while (n && n->addr != a) n = n->next;
    if (!n) {
        n = calloc(1, sizeof *n); n->addr = a;
        pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&n->m, &at);
        pthread_mutexattr_destroy(&at);
        n->next = cs_tab[b]; cs_tab[b] = n;
    }
    pthread_mutex_unlock(&cs_lock);
    return &n->m;
}
SHIM(InitializeCriticalSection) { memset(MEM + ARG(c, 0), 0, 24); cs_get(ARG(c, 0)); RET(c, 0, 1); }
SHIM(InitializeCriticalSectionAndSpinCount) { memset(MEM + ARG(c, 0), 0, 24); cs_get(ARG(c, 0)); RET(c, 1, 2); }
SHIM(DeleteCriticalSection) { RET(c, 0, 1); }
SHIM(EnterCriticalSection) {
    pthread_mutex_t *mutex = cs_get(ARG(c, 0));
#ifdef __ANDROID__
    uint64_t next_report=host_uptime_ns()+5000000000ull;
    while (pthread_mutex_trylock(mutex) == EBUSY) {
        // A lock owner may need the graphics context before it can unlock.
        // Keep uncontended entries cheap and yield immediately on contention.
        extern void host_graphics_yield(void) __attribute__((weak));
        if(host_graphics_yield)host_graphics_yield();
        rt_poll(c, G32(REG(c, R_ESP)));
        uint64_t report_now=host_uptime_ns();
        if(report_now>=next_report){rt_stack_report(c,"critical-section",ARG(c,0));next_report=report_now+5000000000ull;}
        usleep(1000);
    }
#else
    pthread_mutex_lock(mutex);
#endif
    RET(c, 0, 1);
}
SHIM(LeaveCriticalSection) { pthread_mutex_unlock(cs_get(ARG(c, 0))); RET(c, 0, 1); }
SHIM(TryEnterCriticalSection) { RET(c, pthread_mutex_trylock(cs_get(ARG(c, 0))) == 0, 1); }

#ifdef __ANDROID__
static void release_thread_locks(void) {
    pthread_mutex_lock(&cs_lock);
    for (size_t i = 0; i < CS_BUCKETS; ++i)
        for (CSNode *node = cs_tab[i]; node; node = node->next)
            while (pthread_mutex_unlock(&node->m) == 0) {}
    pthread_mutex_unlock(&cs_lock);
}
#endif

// interlocked: real atomics on guest memory
#define A32(a) ((_Atomic uint32_t *)(void *)(MEM + (a)))
SHIM(InterlockedIncrement) { RET(c, __atomic_add_fetch((uint32_t *)(MEM + ARG(c, 0)), 1, __ATOMIC_SEQ_CST), 1); }
SHIM(InterlockedDecrement) { RET(c, __atomic_sub_fetch((uint32_t *)(MEM + ARG(c, 0)), 1, __ATOMIC_SEQ_CST), 1); }
SHIM(InterlockedExchange) { RET(c, __atomic_exchange_n((uint32_t *)(MEM + ARG(c, 0)), ARG(c, 1), __ATOMIC_SEQ_CST), 2); }
SHIM(InterlockedExchangeAdd) { RET(c, __atomic_fetch_add((uint32_t *)(MEM + ARG(c, 0)), ARG(c, 1), __ATOMIC_SEQ_CST), 2); }
SHIM(InterlockedCompareExchange) {
    uint32_t exp = ARG(c, 2);
    __atomic_compare_exchange_n((uint32_t *)(MEM + ARG(c, 0)), &exp, ARG(c, 1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    RET(c, exp, 3);
}

// ---- threads ---------------------------------------------------------------------------------
#ifndef __ANDROID__
typedef struct { uint32_t start, param; Obj *obj; uint32_t tid; } ThreadStart;
static uint32_t next_tid = 2;
extern int host_run_guest(CPU *c, uint32_t entry);   // host.c: trap-guarded call

static void *thread_main(void *p) {
    ThreadStart *ts = p;
    if (!getenv("FNV_NO_QOS")) pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);   // game workers
    pthread_mutex_lock(&ts->obj->m);                      // CREATE_SUSPENDED: wait for ResumeThread
    while (ts->obj->suspend > 0) pthread_cond_wait(&ts->obj->cv, &ts->obj->m);
    pthread_mutex_unlock(&ts->obj->m);
    CPU *c = calloc(1, sizeof *c);
    host_thread_init(c, 1 << 20);
    P32(REG(c, 272) + 0x24, ts->tid);
    ST(c->r + 4256, 0x027F, 2);
    uint32_t esp = REG(c, R_ESP) - 8;
    P32(esp, HOST_EXIT_THUNK); P32(esp + 4, ts->param);
    SETREG(c, R_ESP, esp);
    host_run_guest(c, ts->start);
    pthread_mutex_lock(&ts->obj->m);
    ts->obj->done = 1; ts->obj->exit_code = REG(c, R_EAX);
    pthread_cond_broadcast(&ts->obj->cv);
    pthread_mutex_unlock(&ts->obj->m);
    return NULL;
}
SHIM(CreateThread) {
    ThreadStart *ts = calloc(1, sizeof *ts);
    ts->start = ARG(c, 2); ts->param = ARG(c, 3); ts->obj = obj_new();
    ts->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
    if (ARG(c, 4) & 4) ts->obj->suspend = 1;
    uint32_t h = handle_new(H_THREAD, ts->obj);
    if (ARG(c, 5)) P32(ARG(c, 5), ts->tid);
    fprintf(stderr, "[thread] CreateThread start %#x param %#x flags %#x -> handle %#x tid %u\n", ts->start, ts->param, ARG(c, 4), h, ts->tid);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th; pthread_create(&th, &at, thread_main, ts); pthread_detach(th);
    RET(c, h, 6);
}
SHIM(ResumeThread) {
    Obj *o = handle_get(ARG(c, 0), H_THREAD);
    fprintf(stderr, "[thread] ResumeThread(%#x) %s\n", ARG(c, 0), o ? "" : "-> invalid handle");
    if (!o) { RET(c, 0xFFFFFFFFu, 1); return; }
    pthread_mutex_lock(&o->m);
    int prev = o->suspend;
    if (o->suspend > 0) o->suspend--;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    RET(c, (uint32_t)prev, 1);
}
// Suspending a running thread asynchronously is not supported; only the count is tracked.
SHIM(SuspendThread) { Obj *o = handle_get(ARG(c, 0), H_THREAD); RET(c, o ? (uint32_t)o->suspend : 0xFFFFFFFFu, 1); }
SHIM(SetThreadPriority) { RET(c, 1, 2); }
SHIM(SetThreadIdealProcessor) { RET(c, 0, 2); }
SHIM(GetExitCodeThread) {
    Obj *o = handle_get(ARG(c, 0), H_THREAD);
    if (o) P32(ARG(c, 1), o->done ? o->exit_code : 259 /* STILL_ACTIVE */);
    RET(c, o != NULL, 2);
}
SHIM(ExitThread) { fprintf(stderr, "[host] ExitThread(%u)\n", ARG(c, 0)); pthread_exit(NULL); }
#else
#include "android_threads.inc"
#endif
SHIM(CloseHandle) {
    uint32_t h = ARG(c, 0);
#ifndef __ANDROID__
    if (handle_type(h) == H_THREAD) {
        fprintf(stderr, "[thread] CloseHandle(%#x) thread, caller %#x\n", h, G32(REG(c, R_ESP)));
        static int dumped;
        if (!dumped++) {
            uint32_t sp = REG(c, R_ESP);
            fprintf(stderr, "[thread]   stack:");
            for (int i = 0, n = 0; i < 2048 && n < 24; i++) { uint32_t v = G32(sp + 4 * (uint32_t)i); if (v >= 0x401000 && v < 0xfdf000) { fprintf(stderr, " %#x", v); n++; } }
            fprintf(stderr, "\n");
        }
    }
#else
    int type = handle_type(h);
    if(type<H_FILE||type>H_THREAD){SETERR(6);RET(c,0,1);return;}
    void *object = handle_get(h, type);
    Obj *o = object;
    if (!o) { SETERR(6); RET(c, 0, 1); return; }
    if(type==H_THREAD)__android_log_print(ANDROID_LOG_INFO,"FNVThread","CloseHandle thread=%#x caller=%#x",h,G32(REG(c,R_ESP)));
    host_file_release(type, object);
#endif
    handle_close(h); RET(c, 1, 1);
}

#ifdef __ANDROID__
#define THREAD_SHIMS E(CreateThread) E(ResumeThread) E(SuspendThread) E(SetThreadPriority) E(SetThreadIdealProcessor) E(GetExitCodeThread) E(ExitThread)
void host_sync_reset(void) {
    host_threads_stop();
    for (uint32_t i = 0; i < MAXH; ++i) {
        Obj *o = htab[i].obj;
        if (o) {
            if (host_file_release(htab[i].type, o)) continue;
            extern int host_mmio_release(int type,void *object);
            if (host_mmio_release(htab[i].type,o)) continue;
        }
    }
    memset(htab, 0, sizeof htab);
    while (allocated_objects) {
        Obj *next = allocated_objects->allocated_next;
        pthread_mutex_destroy(&allocated_objects->m);
        pthread_cond_destroy(&allocated_objects->cv);
        free(allocated_objects);
        allocated_objects = next;
    }
    for (size_t i = 0; i < CS_BUCKETS; ++i) {
        CSNode *node = cs_tab[i];
        while (node) {
            CSNode *next = node->next;
            while (pthread_mutex_unlock(&node->m) == 0) {}
            pthread_mutex_destroy(&node->m);
            free(node);
            node = next;
        }
        cs_tab[i] = NULL;
    }
    rt_workers_reset();
}

uint32_t host_shim_test_call(ShimFn fn, const uint32_t *arguments, int count) {
    CPU cpu = {0};
    const uint32_t stack = 0x2fff0000u;
    SETREG(&cpu, R_ESP, stack);
    P32(stack, HOST_EXIT_THUNK);
    for (int i = 0; i < count; ++i) P32(stack + 4 + 4 * i, arguments[i]);
    fn(&cpu);
    if (REG(&cpu, R_ESP) != stack + 4 + 4 * count) return 0xfffffffeu;
    return REG(&cpu, R_EAX);
}

static void *long_wait_signal(void *pointer) {
    Obj *event=pointer;usleep(1100000);pthread_mutex_lock(&event->m);
    event->signaled=1;pthread_cond_broadcast(&event->cv);pthread_mutex_unlock(&event->m);return NULL;
}
int host_sync_selftest(void) {
    // Asset-loading waits routinely exceed one second. Preserve the requested
    // timeout while rt_poll still enforces trace cancellation and its deadline.
    Obj *delayed=obj_new();uint32_t delayed_handle=handle_new(H_EVENT,delayed);pthread_t signaler;
    if(pthread_create(&signaler,NULL,long_wait_signal,delayed))return 20;
    uint32_t delayed_result=wait_one(delayed_handle,2000);pthread_join(signaler,NULL);
    if(delayed_result)return 21;
    uint32_t scratch = gpages(0x1000);
    uint32_t cs[] = {scratch, 0};
    if (host_shim_test_call(InitializeCriticalSectionAndSpinCount_, cs, 2) != 1) return 1;
    host_shim_test_call(EnterCriticalSection_, cs, 1);
    if (host_shim_test_call(TryEnterCriticalSection_, cs, 1) != 1) return 2;
    host_shim_test_call(LeaveCriticalSection_, cs, 1);
    host_shim_test_call(LeaveCriticalSection_, cs, 1);

    uint32_t semaphore_args[] = {0, 1, 1, 0};
    uint32_t semaphore = host_shim_test_call(CreateSemaphoreA_, semaphore_args, 4);
    uint32_t release_args[] = {semaphore, 1, 0};
    if (host_shim_test_call(ReleaseSemaphore_, release_args, 3) != 0) return 3;

    uint32_t event_args[] = {0, 0, 1, 0};
    uint32_t event = host_shim_test_call(CreateEventA_, event_args, 4);
    event_args[2] = 0;
    uint32_t second = host_shim_test_call(CreateEventA_, event_args, 4);
    P32(scratch + 64, event); P32(scratch + 68, second);
    uint32_t wait_all[] = {2, scratch + 64, 1, 0};
    if (host_shim_test_call(WaitForMultipleObjects_, wait_all, 4) != 0x102) return 4;
    uint32_t wait_one_args[] = {event, 0};
    if (host_shim_test_call(WaitForSingleObject_, wait_one_args, 2) != 0) return 5;
    if (host_shim_test_call(WaitForSingleObject_, wait_one_args, 2) != 0x102) return 6;
    uint32_t event_handle[] = {event};
    uint32_t second_handle[] = {second};
    host_shim_test_call(SetEvent_, event_handle, 1);
    host_shim_test_call(SetEvent_, second_handle, 1);
    if (host_shim_test_call(WaitForMultipleObjects_, wait_all, 4) != 0) return 7;

    uint32_t mutex_args[] = {0, 1, 0};
    uint32_t mutex = host_shim_test_call(CreateMutexA_, mutex_args, 3);
    wait_one_args[0] = mutex;
    if (host_shim_test_call(WaitForSingleObject_, wait_one_args, 2) != 0) return 8;
    uint32_t mutex_handle[] = {mutex};
    if (!host_shim_test_call(ReleaseMutex_, mutex_handle, 1) ||
        !host_shim_test_call(ReleaseMutex_, mutex_handle, 1) ||
        host_shim_test_call(ReleaseMutex_, mutex_handle, 1) != 0) return 9;
    host_sync_reset();
    return 0;
}
#else
#define THREAD_SHIMS E(CreateThread) E(ResumeThread) E(SuspendThread) E(SetThreadPriority) \
    E(SetThreadIdealProcessor) E(GetExitCodeThread) E(ExitThread)
#endif

SHIM_TABLE(sync_shims,
    E(CreateEventA) E(SetEvent) E(ResetEvent) E(CreateSemaphoreA) E(ReleaseSemaphore) E(CreateMutexA) E(ReleaseMutex)
    E(WaitForSingleObject) E(WaitForMultipleObjects) E(InitializeCriticalSection)
    E(InitializeCriticalSectionAndSpinCount) E(DeleteCriticalSection) E(EnterCriticalSection)
    E(LeaveCriticalSection) E(TryEnterCriticalSection) E(InterlockedIncrement) E(InterlockedDecrement)
    E(InterlockedExchange) E(InterlockedExchangeAdd) E(InterlockedCompareExchange)
    THREAD_SHIMS E(CloseHandle))
