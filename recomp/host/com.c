#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include "com.h"
static pthread_mutex_t tl = PTHREAD_MUTEX_INITIALIZER;

uint32_t host_thunk(const char *name, ShimFn fn) {
#ifdef __ANDROID__
    uint32_t thunk = host_register_import("com", name, fn);
    if (!thunk) abort();
    return thunk;
#else
    pthread_mutex_lock(&tl);
    if (host_import_count >= MAX_IMPORTS) { fprintf(stderr, "[com] out of thunks\n"); abort(); }
    HostImport *hi = &host_imports[host_import_count];
    snprintf(hi->dll, sizeof hi->dll, "com");
    snprintf(hi->name, sizeof hi->name, "%s", name);
    hi->fn = fn;
    uint32_t r = IMPORT_THUNK_BASE + 4u * (uint32_t)host_import_count++;
    pthread_mutex_unlock(&tl);
    return r;
#endif
}

#ifndef __ANDROID__
// Unimplemented COM method on macOS. Android's dispatcher traps null entries.
static _Thread_local const char *missing_name;
static void com_missing(CPU *c) {
    // find our own name from the thunk table entry that dispatched here
    fprintf(stderr, "[com] unimplemented method called (see last [imp] trace line)\n");
    rt_trap(c, 8 /* TRAP_IMPORT */, 0);
}
#endif

uint32_t com_vtable(const char *iface, const ComMethod *m, int n) {
    uint32_t vt = gmalloc(4u * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        char nm[96];
        snprintf(nm, sizeof nm, "%s::%s", iface, m[i].name ? m[i].name : "?");
        ShimFn function = m[i].fn;
#ifndef __ANDROID__
        if (!function) function = com_missing;
#endif
        P32(vt + 4u * (uint32_t)i, host_thunk(nm, function));
    }
#ifndef __ANDROID__
    (void)missing_name;
#endif
    return vt;
}

uint32_t com_new(uint32_t vtable, uint32_t extra) {
    uint32_t o = gmalloc(12 + extra);
    P32(o, vtable); P32(o + 4, 1);
    return o;
}

void com_AddRef(CPU *c) { uint32_t o = COM_THIS(c); uint32_t r = __atomic_add_fetch((uint32_t *)(void *)(MEM + o + 4), 1, __ATOMIC_SEQ_CST); RET(c, r, 1); }
void com_Release(CPU *c) {
    uint32_t *reference = (uint32_t *)(void *)(MEM + COM_THIS(c) + 4);
    uint32_t previous = __atomic_load_n(reference, __ATOMIC_SEQ_CST);
    while (previous && !__atomic_compare_exchange_n(reference, &previous, previous - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
    RET(c, previous ? previous - 1 : 0, 1);
}
