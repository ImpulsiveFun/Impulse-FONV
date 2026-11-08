/* Included by shims_sync.c: uses its waitable-object and handle implementation. */
static void CloseHandle_(CPU *cpu);
void host_sync_reset(void);
extern uint32_t rt_trap_addr(void);
extern int64_t rt_trace_branch_budget(void);
typedef struct AndroidThread {
    struct AndroidThread *next;
    pthread_t thread;
    Obj *object;
    uint32_t start, parameter, stack_size, tid;
    int initialized;
    GuestFn test_callback;
} AndroidThread;
static AndroidThread *android_threads;
static unsigned android_thread_count;
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static int thread_selftest_enabled;
static void thread_test_callback(CPU *cpu);

static void *android_thread_main(void *argument) {
    AndroidThread *thread = argument;
    CPU *cpu = calloc(1, sizeof *cpu);
    host_thread_init(cpu, thread->stack_size);
    ST(cpu->r + 4256, 0x027f, 2);
    uint32_t stack = REG(cpu, R_ESP) - 8;
    P32(stack, HOST_EXIT_THUNK); P32(stack + 4, thread->parameter);
    SETREG(cpu, R_ESP, stack);
    rt_worker_begin();
    char name[16];snprintf(name,sizeof name,"fnv-%08x",thread->start);
    pthread_setname_np(pthread_self(),name);
    __android_log_print(ANDROID_LOG_INFO,"FNVThread","start=%#x parameter=%#x",thread->start,thread->parameter);
    pthread_mutex_lock(&thread->object->m);
    thread->tid = G32(REG(cpu, 272) + 0x24);
    thread->initialized = 1;
    pthread_cond_broadcast(&thread->object->cv);
    if(thread->object->suspend)__android_log_print(ANDROID_LOG_INFO,"FNVThread","suspended start=%#x count=%d tid=%u",thread->start,thread->object->suspend,thread->tid);
    while (thread->object->suspend && !rt_workers_stopping()) {
        struct timespec until; deadline(&until, 20);
        pthread_cond_timedwait(&thread->object->cv, &thread->object->m, &until);
    }
    pthread_mutex_unlock(&thread->object->m);
    __android_log_print(ANDROID_LOG_INFO,"FNVThread","running start=%#x tid=%u stopping=%d",thread->start,thread->tid,rt_workers_stopping());
    int trap = TRAP_CANCELLED;
    uint32_t exit_code = 0;
    if (!rt_workers_stopping()) {
        // Asset loading can exhaust the old fixed worker allowance in seconds.
        // Use the selected trace allowance; deadline and cancellation checks stay active.
        trap = thread->test_callback ? rt_run_callback(cpu, thread->test_callback, 200000000) :
                                      rt_run_entry(cpu, thread->start, rt_trace_branch_budget());
        exit_code = trap == TRAP_THREAD_EXIT ? rt_trap_addr() : REG(cpu, R_EAX);
        if (!trap && REG(cpu, R_ESP) != stack + 8) {
            trap = TRAP_FALLOUT;
            rt_worker_failure(trap, thread->start);
        } else if (trap && trap != TRAP_CANCELLED && trap != TRAP_THREAD_EXIT) {
            rt_worker_failure(trap, rt_trap_addr());
        }
    }
    release_thread_locks();
    __android_log_print(ANDROID_LOG_INFO,"FNVThread","exit start=%#x trap=%d address=%#x",thread->start,trap,rt_trap_addr());
    pthread_mutex_lock(&thread->object->m);
    thread->object->done = 1;
    thread->object->exit_code = exit_code;
    pthread_cond_broadcast(&thread->object->cv);
    pthread_mutex_unlock(&thread->object->m);
    free(cpu);
    return NULL;
}

static void host_threads_stop(void) {
    rt_workers_stop();
    release_thread_locks();
    pthread_mutex_lock(&thread_lock);
    AndroidThread *threads = android_threads;
    pthread_mutex_unlock(&thread_lock);
    for (AndroidThread *thread = threads; thread; thread = thread->next) {
        pthread_mutex_lock(&thread->object->m);
        pthread_cond_broadcast(&thread->object->cv);
        pthread_mutex_unlock(&thread->object->m);
    }
    while (threads) {
        AndroidThread *next = threads->next;
        pthread_join(threads->thread, NULL);
        free(threads);
        threads = next;
    }
    pthread_mutex_lock(&thread_lock);
    android_threads = NULL; android_thread_count = 0;
    pthread_mutex_unlock(&thread_lock);
}

SHIM(CreateThread) {
    uint32_t flags = ARG(c, 4), stack_size = ARG(c, 1);
    if (flags & ~0x10004u || stack_size > 64u * 1024 * 1024) { SETERR(87); RET(c, 0, 6); return; }
    pthread_mutex_lock(&thread_lock);
    if (rt_workers_stopping() || android_thread_count >= 128) {
        pthread_mutex_unlock(&thread_lock); SETERR(8); RET(c, 0, 6); return;
    }
    AndroidThread *thread = calloc(1, sizeof *thread);
    if (!thread) { pthread_mutex_unlock(&thread_lock); SETERR(8); RET(c, 0, 6); return; }
    thread->object = obj_new();
    thread->start = ARG(c, 2); thread->parameter = ARG(c, 3);
    thread->stack_size = stack_size ? (stack_size + 4095u) & ~4095u : 1024 * 1024;
    thread->object->suspend = (flags & 4) != 0;
    if (thread_selftest_enabled && thread->start == 0xfedf1000u) thread->test_callback = thread_test_callback;
    uint32_t handle = handle_new(H_THREAD, thread->object);
    pthread_attr_t attributes; pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u * 1024 * 1024);
    int error = handle ? pthread_create(&thread->thread, &attributes, android_thread_main, thread) : EAGAIN;
    pthread_attr_destroy(&attributes);
    if (error) {
        handle_close(handle); free(thread); pthread_mutex_unlock(&thread_lock);
        SETERR(8); RET(c, 0, 6); return;
    }
    thread->next = android_threads; android_threads = thread; ++android_thread_count;
    pthread_mutex_unlock(&thread_lock);
    pthread_mutex_lock(&thread->object->m);
    while (!thread->initialized) pthread_cond_wait(&thread->object->cv, &thread->object->m);
    uint32_t tid = thread->tid;
    pthread_mutex_unlock(&thread->object->m);
    if (ARG(c, 5)) P32(ARG(c, 5), tid);
    __android_log_print(ANDROID_LOG_INFO,"FNVThread","created handle=%#x start=%#x flags=%#x tid=%u",handle,thread->start,flags,tid);
    RET(c, handle, 6);
}
SHIM(ResumeThread) {
    Obj *object = handle_get(ARG(c, 0), H_THREAD);
    if (!object) { __android_log_print(ANDROID_LOG_WARN,"FNVThread","ResumeThread invalid handle=%#x caller=%#x",ARG(c,0),G32(REG(c,R_ESP)));SETERR(6); RET(c, 0xffffffffu, 1); return; }
    pthread_mutex_lock(&object->m);
    int previous = object->suspend;
    if (previous) --object->suspend;
    pthread_cond_broadcast(&object->cv);
    pthread_mutex_unlock(&object->m);
    __android_log_print(ANDROID_LOG_INFO,"FNVThread","ResumeThread handle=%#x previous=%d",ARG(c,0),previous);
    RET(c, previous, 1);
}
SHIM(SuspendThread) {
    Obj *object = handle_get(ARG(c, 0), H_THREAD);
    if (!object) { SETERR(6); RET(c, 0xffffffffu, 1); return; }
    pthread_mutex_lock(&object->m);
    int previous = object->suspend;
    if (previous && !object->done) ++object->suspend;
    pthread_mutex_unlock(&object->m);
    if (!previous) SETERR(50); /* asynchronous suspension is not implemented */
    RET(c, previous ? (uint32_t)previous : 0xffffffffu, 1);
}
SHIM(SetThreadPriority) { int ok = ARG(c, 0) == 0xfffffffeu || handle_get(ARG(c, 0), H_THREAD); if (!ok) SETERR(6); RET(c, ok, 2); }
SHIM(SetThreadIdealProcessor) { int ok = ARG(c, 0) == 0xfffffffeu || handle_get(ARG(c, 0), H_THREAD); if (!ok) SETERR(6); RET(c, ok ? 0 : 0xffffffffu, 2); }
SHIM(GetExitCodeThread) {
    Obj *object = handle_get(ARG(c, 0), H_THREAD);
    if (!object || !ARG(c, 1)) { SETERR(6); RET(c, 0, 2); return; }
    pthread_mutex_lock(&object->m);
    uint32_t code = object->done ? object->exit_code : 259;
    pthread_mutex_unlock(&object->m);
    P32(ARG(c, 1), code); RET(c, 1, 2);
}
SHIM(ExitThread) { rt_trap(c, TRAP_THREAD_EXIT, ARG(c, 0)); }

static uint32_t thread_test_call(CPU *cpu, const char *name, uint32_t *arguments, int count) {
    uint32_t original = REG(cpu, R_ESP), stack = original - 128;
    P32(stack, HOST_EXIT_THUNK);
    for (int i = 0; i < count; ++i) P32(stack + 4 + i * 4, arguments[i]);
    SETREG(cpu, R_ESP, stack);
    host_find_shim("KERNEL32.dll", name)(cpu);
    uint32_t result = REG(cpu, R_EAX);
    SETREG(cpu, R_ESP, original);
    return result;
}
static void thread_test_callback(CPU *cpu) {
    uint32_t data = ARG(cpu, 0), teb = REG(cpu, 272), slot[] = {63, data};
    uint32_t block = G32(G32(teb + 0x2c));
    int ok = teb != G32(data) && G32(teb + 0x24) != G32(data + 4) && block != G32(data + 8);
    ok &= block && !memcmp(MEM + block, host_tls_template, host_tls_template_size);
    ok &= thread_test_call(cpu, "TlsGetValue", slot, 1) == 0;
    ok &= thread_test_call(cpu, "TlsSetValue", slot, 2) == 1;
    uint32_t error[] = {33}, sleep[] = {2};
    thread_test_call(cpu, "SetLastError", error, 1);
    thread_test_call(cpu, "Sleep", sleep, 1);
    ok &= thread_test_call(cpu, "GetLastError", error, 0) == 33;
    ok &= thread_test_call(cpu, "TlsGetValue", slot, 1) == data;
    P32(data + 12, ok ? 1 : 2);
    RET(cpu, 0x12345678, 1);
}

int host_threads_selftest(CPU *main) {
    CPU saved = *main;
    uint32_t saved_error = host_last_error, teb = REG(main, 272), saved_tls = G32(teb + 0xe10 + 63 * 4);
    P32(teb + 0xe10 + 63 * 4, 0x76543210);
    thread_selftest_enabled = 1;
    int failure = 1;
    do {
        uint32_t handles[2];
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t data = 0x230000 + 0x100 * i;
            P32(data, teb); P32(data + 4, G32(teb + 0x24));
            P32(data + 8, G32(G32(teb + 0x2c))); P32(data + 12, 0);
            uint32_t create[] = {0, 0, 0xfedf1000u, data, 4, data + 16};
            handles[i] = host_shim_test_call(CreateThread_, create, 6);
            if (!handles[i]) { failure = 2; goto finished; }
            uint32_t wait[] = {handles[i], 0};
            if (host_shim_test_call(WaitForSingleObject_, wait, 2) != 258 || G32(data + 12)) { failure = 3; goto finished; }
        }
        host_last_error = 55;
        for (unsigned i = 0; i < 2; ++i)
            if (host_shim_test_call(ResumeThread_, &handles[i], 1) != 1) { failure = 4; goto finished; }
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t data = 0x230000 + 0x100 * i, wait[] = {handles[i], 1000}, code[] = {handles[i], data + 20};
            if (host_shim_test_call(WaitForSingleObject_, wait, 2) || G32(data + 12) != 1) { failure = 5; goto finished; }
            if (!host_shim_test_call(GetExitCodeThread_, code, 2) || G32(data + 20) != 0x12345678) { failure = 6; goto finished; }
            host_shim_test_call(CloseHandle_, &handles[i], 1);
        }
        if (host_last_error != 55 || G32(teb + 0xe10 + 63 * 4) != 0x76543210) { failure = 7; break; }
        uint32_t create[] = {0, 0, 0xfedf1000u, 0x230000, 4, 0};
        uint32_t suspended = host_shim_test_call(CreateThread_, create, 6);
        if (!suspended) { failure = 8; break; }
        host_shim_test_call(CloseHandle_, &suspended, 1);
        failure = 0;
    } while (0);
finished:
    host_sync_reset();
    thread_selftest_enabled = 0;
    P32(teb + 0xe10 + 63 * 4, saved_tls);
    host_last_error = saved_error;
    *main = saved;
    return failure;
}
