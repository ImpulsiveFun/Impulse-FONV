#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "host/shims.h"
#include "host/input.h"
#include "host/d3d9_backend.h"
extern D3DState g_d3d;

extern uint8_t *rt_mem(void);
extern int rt_count(void);
extern uint32_t rt_addr(int i);
extern int rt_run(int i, CPU *cpu, int64_t budget);
extern uint32_t rt_trap_addr(void);
extern void rt_trace_timeout_ms(unsigned ms);
extern int64_t rt_trace_branch_budget(void);
extern void rt_request_trace_stop(void);

struct ImageSegment { uint32_t address, offset, size; };
struct ImageImport { uint32_t slot; const char *name; };
#include "startup_image.h"

HostImport host_imports[MAX_IMPORTS];
_Atomic int host_import_count;
uint32_t host_image_base;
uint8_t *host_tls_template;
uint32_t host_tls_template_size, host_tls_block_size;
extern void gheap_reset(void);
extern int fnv_prepare_script_commands(void);
extern void host_kernel32_reset(void);
extern void host_sync_reset(void);
extern int host_sync_selftest(void);
extern int host_text_selftest(void);
extern int host_memory_selftest(char *message, size_t capacity);
extern int host_initializer_selftest(char *message, size_t capacity);
extern int host_math_trap_selftest(void);
extern int host_fmod_selftest(void);
extern int host_d3d_profile_selftest(void);
extern int host_actor_value_selftest(void);
extern int host_clip_plane_selftest(void);
extern const char *host_d3d_adapter_description(void);
JNIEXPORT jstring JNICALL Java_org_fnvprobe_ProbeActivity_virtualAdapterDescription(JNIEnv *env,jclass cls){
    (void)cls;return (*env)->NewStringUTF(env,host_d3d_adapter_description());
}
extern int host_files_selftest(void);
extern int host_mmio_selftest(void);
extern int host_validate_base_assets(char *message, size_t capacity);
extern int host_threads_selftest(CPU *cpu);
extern void host_window_configure(uint32_t width, uint32_t height);
extern void host_win_reset(void);
extern int host_win_selftest(void);
extern void host_dinput_reset(void);
extern int host_dinput_selftest(void);
extern void host_dsound_reset(void);
extern void host_video_reset(void);
extern int host_media_bind(JNIEnv *env);
extern int host_media_selftest(JNIEnv *env);
extern int host_media_com_selftest(void);
extern void host_media_reset(void);
extern void host_vorbis_reset(void);
extern int host_vorbis_selftest(void);
extern int android_audio_selftest(void);
extern void host_graphics_reset(void);
extern void host_graphics_enter(void);
extern void host_graphics_leave(void);
extern void host_graphics_yield(void);
extern void host_graphics_retain(int enabled);
extern int host_graphics_capture(D3DState *s,const char *path);
extern void host_d3d9_reset(void);
extern void host_d3dx_reset(void);
extern int host_d3dx_selftest(void);
extern int host_graphics_selftest(void);
extern int host_graphics_thread_selftest(void);
extern const char *host_graphics_error(void);

JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_configureWindow(
        JNIEnv *env, jclass cls, jint width, jint height) {
    (void)env; (void)cls;
    host_window_configure((uint32_t)width, (uint32_t)height);
}
extern int host_virtual_selftest(void);

static _Atomic unsigned handled_imports, graphics_failures;
static _Atomic int debug_input_script;
static _Atomic int play_closing;
_Thread_local uint32_t host_current_import_address;
static pthread_mutex_t import_registration_lock = PTHREAD_MUTEX_INITIALIZER;
enum { IMPORT_GRAPHICS=1, IMPORT_GRAPHICS_REPORT=2, IMPORT_MEDIA_LOG=4, IMPORT_TRACK_ADDRESS=8, IMPORT_RETAIN_CONTEXT=16 };
static uint8_t import_flags[MAX_IMPORTS];

static uint8_t classify_import(const HostImport *entry) {
    uint8_t flags = 0;
    if (strstr(entry->name,"IDirect3D") || strstr(entry->name,"D3DX") || strstr(entry->dll,"d3d")) {
        flags |= IMPORT_GRAPHICS;
        if (!strstr(entry->name,"::Check")) flags |= IMPORT_GRAPHICS_REPORT;
    }
    if (!strcmp(entry->dll,"com") && (strstr(entry->name,"graph_m::") || strstr(entry->name,"control_m::")))
        flags |= IMPORT_MEDIA_LOG;
    if (!strcmp(entry->name,"_BinkClose@4")) flags |= IMPORT_TRACK_ADDRESS;
    if (!strcmp(entry->name,"EnterCriticalSection") || !strcmp(entry->name,"LeaveCriticalSection") ||
        !strcmp(entry->name,"TryEnterCriticalSection")) flags |= IMPORT_RETAIN_CONTEXT;
    if (!strcmp(entry->dll,"KERNEL32.dll") || !strcmp(entry->dll,"kernel32.dll")) {
        static const char *const nonblocking[] = {
            "GetTickCount", "QueryPerformanceCounter", "QueryPerformanceFrequency",
            "TlsGetValue", "TlsSetValue", "GetCurrentThreadId", "GetCurrentProcessId",
            "InterlockedIncrement", "InterlockedDecrement", "InterlockedExchange",
            "InterlockedExchangeAdd", "InterlockedCompareExchange",
        };
        for (unsigned i=0;i<sizeof nonblocking/sizeof nonblocking[0];++i)
            if (!strcmp(entry->name,nonblocking[i])) flags |= IMPORT_RETAIN_CONTEXT;
    }
    return flags;
}

uint32_t host_register_import(const char *dll, const char *name, ShimFn function) {
    pthread_mutex_lock(&import_registration_lock);
    for (int i = 0; i < host_import_count; ++i) {
        if (host_imports[i].fn == function &&
            (strcmp(dll, "com") || !strcmp(host_imports[i].name, name))) {
            pthread_mutex_unlock(&import_registration_lock);
            return IMPORT_THUNK_BASE + 4u * i;
        }
    }
    int index = host_import_count;
    if (index >= MAX_IMPORTS) { pthread_mutex_unlock(&import_registration_lock); return 0; }
    HostImport *entry = &host_imports[index];
    snprintf(entry->dll, sizeof entry->dll, "%s", dll);
    snprintf(entry->name, sizeof entry->name, "%s", name);
    entry->fn = function;
    import_flags[index] = classify_import(entry);
    host_import_count = index + 1;
    pthread_mutex_unlock(&import_registration_lock);
    return IMPORT_THUNK_BASE + 4u * index;
}

void rt_import_call(CPU *cpu, uint32_t address) {
    uint32_t index = (address - IMPORT_THUNK_BASE) / 4;
    if (index >= (uint32_t)host_import_count || !host_imports[index].fn)
        rt_trap(cpu, TRAP_IMPORT, address);
    const HostImport *entry = &host_imports[index];
    uint8_t flags = import_flags[index];
    if(flags & IMPORT_MEDIA_LOG)
        __android_log_print(ANDROID_LOG_INFO,"FNVMedia","%s caller=%#x args=%#x,%#x,%#x",entry->name,G32(REG(cpu,R_ESP)),ARG(cpu,0),ARG(cpu,1),ARG(cpu,2));
    if(flags & IMPORT_GRAPHICS)host_graphics_enter();
    else if(!(flags & IMPORT_RETAIN_CONTEXT))host_graphics_yield();
    if(flags & IMPORT_TRACK_ADDRESS) {
        uint32_t previous_import = host_current_import_address;
        host_current_import_address = address;
        entry->fn(cpu);
        host_current_import_address = previous_import;
    } else entry->fn(cpu);
    if(flags & IMPORT_GRAPHICS)host_graphics_leave();
    atomic_fetch_add_explicit(&handled_imports,1,memory_order_relaxed);
    if ((flags & IMPORT_GRAPHICS_REPORT) && (REG(cpu,R_EAX)&0x80000000u) && graphics_failures++ < 128)
        __android_log_print(ANDROID_LOG_WARN,"FNVGraphics","%s!%s returned %#x",entry->dll,entry->name,REG(cpu,R_EAX));
}

JNIEXPORT jstring JNICALL Java_org_fnvprobe_ProbeActivity_runStartup(
        JNIEnv *env, jclass cls, jbyteArray file, jstring digest, jstring game_root, jint seconds) {
    (void)cls;
    if(!host_media_bind(env))return (*env)->NewStringUTF(env,"Unable to initialize the Android music bridge.");
    const char *hash = (*env)->GetStringUTFChars(env, digest, NULL);
    if (!hash) return NULL;
    int matches = strcmp(hash, STARTUP_SHA256) == 0;
    (*env)->ReleaseStringUTFChars(env, digest, hash);
    if (!matches) return (*env)->NewStringUTF(env, "Select the exact executable used to build this APK.");
    const char *root = (*env)->GetStringUTFChars(env, game_root, NULL);
    if (!root) return NULL;
    int root_error = setenv("FNV_ROOT", root, 1);
    (*env)->ReleaseStringUTFChars(env, game_root, root);
    if (root_error) return (*env)->NewStringUTF(env, "Unable to configure the game folder.");
    char asset_failure[256];
    if (!host_validate_base_assets(asset_failure, sizeof asset_failure))
        return (*env)->NewStringUTF(env, asset_failure);

    uint8_t *memory = rt_mem();
    if (!memory) return (*env)->NewStringUTF(env, "Unable to map guest memory.");
    host_sync_reset();
    host_dsound_reset();
    host_video_reset();
    host_media_reset();
    host_vorbis_reset();
    host_graphics_reset();
    host_d3d9_reset();
    host_d3dx_reset();
    host_win_reset();
    host_dinput_reset();
    memset(memory + STARTUP_BASE, 0, STARTUP_SIZE);
    for (size_t i = 0; i < sizeof startup_segments / sizeof startup_segments[0]; ++i) {
        struct ImageSegment segment = startup_segments[i];
        (*env)->GetByteArrayRegion(env, file, segment.offset, segment.size,
                                  (jbyte *)(memory + segment.address));
        if ((*env)->ExceptionCheck(env)) return NULL;
    }
    host_tls_template = NULL;
    host_tls_template_size = host_tls_block_size = 0;
    uint32_t pe = STARTUP_BASE + G32(STARTUP_BASE + 0x3c);
    uint32_t tls = G32(pe + 24 + 96 + 9 * 8);
    if (tls) {
        if ((uint64_t)tls + 24 > STARTUP_SIZE)
            return (*env)->NewStringUTF(env, "TLS directory lies outside the executable image.");
        uint32_t directory = STARTUP_BASE + tls;
        uint32_t start = G32(directory), end = G32(directory + 4);
        uint32_t slot = G32(directory + 8), zeros = G32(directory + 16);
        if (start < STARTUP_BASE || end < start ||
            (uint64_t)end > (uint64_t)STARTUP_BASE + STARTUP_SIZE ||
            slot < STARTUP_BASE || (uint64_t)slot + 4 > (uint64_t)STARTUP_BASE + STARTUP_SIZE ||
            (uint64_t)end - start + zeros > 1024 * 1024)
            return (*env)->NewStringUTF(env, "Invalid TLS template or index in the executable.");
        host_tls_template = memory + start;
        host_tls_template_size = end - start;
        host_tls_block_size = host_tls_template_size + zeros;
        P32(slot, 0);
    }
    host_import_count = sizeof startup_imports / sizeof startup_imports[0];
    host_image_base = STARTUP_BASE;
    memset(host_imports, 0, sizeof host_imports);
    for (int i = 0; i < host_import_count; ++i) {
        HostImport *import = &host_imports[i];
        const char *label = startup_imports[i].name;
        const char *separator = strchr(label, '!');
        snprintf(import->dll, sizeof import->dll, "%.*s", (int)(separator - label), label);
        snprintf(import->name, sizeof import->name, "%s", separator + 1);
        import->fn = host_find_shim(import->dll, import->name);
        import_flags[i] = classify_import(import);
        ST(memory + startup_imports[i].slot, IMPORT_THUNK_BASE + 4 * i, 4);
    }
    host_sync_reset();
    gheap_reset();
    if (!fnv_prepare_script_commands())
        return (*env)->NewStringUTF(env, "Could not initialize the Android script command table.");
    host_kernel32_reset();
    char *arguments[] = {"FalloutNV.exe"};
    host_init_args(1, arguments);
    CPU cpu = {0};
    host_thread_init(&cpu, 0x10000);
    if (host_tls_template) {
        uint32_t block = G32(G32(REG(&cpu, 272) + 0x2c));
        if (!block || memcmp(memory + block, host_tls_template, host_tls_template_size) != 0)
            return (*env)->NewStringUTF(env, "Per-thread data initialization check failed.");
        for (uint32_t i = host_tls_template_size; i < host_tls_block_size; ++i)
            if (memory[block + i] != 0)
                return (*env)->NewStringUTF(env, "Per-thread zero-fill check failed.");
    }
    uint32_t stack = REG(&cpu, R_ESP) - 4;
    ST(memory + stack, HOST_EXIT_THUNK, 4);
    ST(cpu.r + R_ESP, stack, 4);
    ST(cpu.r + 4256, 0x027fu, 2);
    char memory_failure[256];
    int fmod_test=host_fmod_selftest();
    if(fmod_test){char failure[96];snprintf(failure,sizeof failure,"CRT remainder checks failed at stage %d.",fmod_test);return (*env)->NewStringUTF(env,failure);}
    if (!host_math_trap_selftest())
        return (*env)->NewStringUTF(env, "CRT math fault-path check failed.");
    if (!host_initializer_selftest(memory_failure, sizeof memory_failure))
        return (*env)->NewStringUTF(env, memory_failure);
    if (!host_memory_selftest(memory_failure, sizeof memory_failure))
        return (*env)->NewStringUTF(env, memory_failure);
    int sync_test = host_sync_selftest();
    if (sync_test) {
        host_sync_reset();
        char failure[96];
        snprintf(failure, sizeof failure, "Synchronization check failed at stage %d.", sync_test);
        return (*env)->NewStringUTF(env, failure);
    }
    int text_test = host_text_selftest();
    if (text_test) {
        char failure[96];
        snprintf(failure, sizeof failure, "Text conversion check failed at stage %d.", text_test);
        return (*env)->NewStringUTF(env, failure);
    }
    int virtual_test = host_virtual_selftest();
    if (virtual_test) {
        char failure[96];
        snprintf(failure, sizeof failure, "Virtual-memory check failed at stage %d.", virtual_test);
        return (*env)->NewStringUTF(env, failure);
    }
    SETERR(0);

    int file_test = host_files_selftest();
    if (file_test) {
        char failure[96];
        snprintf(failure, sizeof failure, "File checks failed at stage %d.", file_test);
        return (*env)->NewStringUTF(env, failure);
    }
    int thread_test = host_threads_selftest(&cpu);
    if (thread_test) {
        char failure[96];
        snprintf(failure, sizeof failure, "Thread checks failed at stage %d.", thread_test);
        return (*env)->NewStringUTF(env, failure);
    }
    int window_test = host_win_selftest();
    if (window_test) {
        char failure[96];
        snprintf(failure, sizeof failure, "Window checks failed at stage %d.", window_test);
        return (*env)->NewStringUTF(env, failure);
    }
    int input_test = host_dinput_selftest();
    if (input_test) {
        char failure[96];
        snprintf(failure, sizeof failure, "Input checks failed at stage %d.", input_test);
        return (*env)->NewStringUTF(env, failure);
    }
    int audio_test = android_audio_selftest();
    if (audio_test) {
        char failure[96]; snprintf(failure, sizeof failure, "Audio checks failed at stage %d.", audio_test);
        return (*env)->NewStringUTF(env, failure);
    }
    int d3dx_test = host_d3dx_selftest();
    if (d3dx_test) {
        char failure[96];snprintf(failure,sizeof failure,"D3DX math checks failed at stage %d.",d3dx_test);
        return (*env)->NewStringUTF(env,failure);
    }
    int profile_test=host_d3d_profile_selftest();
    if(profile_test){char failure[96];snprintf(failure,sizeof failure,"Game shader capability selection failed at stage %d.",profile_test);return (*env)->NewStringUTF(env,failure);}
    int clip_plane_test=host_clip_plane_selftest();
    if(clip_plane_test){char failure[96];snprintf(failure,sizeof failure,"Clip-plane API checks failed at stage %d.",clip_plane_test);return (*env)->NewStringUTF(env,failure);}
    int actor_value_test=host_actor_value_selftest();
    if(actor_value_test){char failure[96];snprintf(failure,sizeof failure,"Actor-value callback checks failed at stage %d.",actor_value_test);return (*env)->NewStringUTF(env,failure);}
    int mmio_test=host_mmio_selftest();
    if(mmio_test){char failure[96];snprintf(failure,sizeof failure,"WAV I/O checks failed at stage %d.",mmio_test);return (*env)->NewStringUTF(env,failure);}
    int media_test=host_media_selftest(env);
    if(!media_test)media_test=host_media_com_selftest();
    if(media_test){char failure[96];snprintf(failure,sizeof failure,"Music decoder checks failed at stage %d.",media_test);return (*env)->NewStringUTF(env,failure);}
    int vorbis_test=host_vorbis_selftest();
    if(vorbis_test){char failure[96];snprintf(failure,sizeof failure,"Vorbis decoder checks failed at stage %d.",vorbis_test);return (*env)->NewStringUTF(env,failure);}
    int thread_graphics_test=host_graphics_thread_selftest();
    if(thread_graphics_test){char failure[96];snprintf(failure,sizeof failure,"Graphics thread checks failed at stage %d.",thread_graphics_test);return (*env)->NewStringUTF(env,failure);}
    int graphics_test = host_graphics_selftest();
    if (graphics_test) {
        char failure[640]; snprintf(failure, sizeof failure, "Graphics checks failed at stage %d: %s", graphics_test, host_graphics_error());
        return (*env)->NewStringUTF(env, failure);
    }
    host_win_reset();
    SETERR(0);

    int index = 0;
    while (index < rt_count() && rt_addr(index) != STARTUP_ENTRY) ++index;
    if (index == rt_count()) return (*env)->NewStringUTF(env, "Startup entry is missing.");
    handled_imports = 0; graphics_failures = 0;
    uint64_t began=host_uptime_ns();
    if(atomic_exchange(&debug_input_script,0) && seconds>0){
        char script[2048];snprintf(script,sizeof script,"%s/.probe-input-script.txt",getenv("FNV_ROOT"));setenv("FNV_INPUT_SCRIPT",script,1);
    }else unsetenv("FNV_INPUT_SCRIPT");
    unsigned trace_ms=seconds>0 ? (unsigned)seconds*1000 : 0;
    rt_trace_timeout_ms(trace_ms);
    host_graphics_retain(1);
    if (!seconds && atomic_load(&play_closing)) rt_request_trace_stop();
    int trap = rt_run(index, &cpu, rt_trace_branch_budget());
    host_graphics_retain(0);
    rt_trace_timeout_ms(30000);
    uint64_t elapsed_ms=(host_uptime_ns()-began)/1000000ull;
    uint32_t address = rt_trap_addr();
    uint64_t frames = g_d3d.frames,draws=g_d3d.draws;
    if(frames){const char *root=getenv("FNV_ROOT");char capture[2048];
        if(root){snprintf(capture,sizeof capture,"%s/.startup-frame.ppm",root);host_graphics_capture(&g_d3d,capture);}}

    host_sync_reset();
    host_dsound_reset();
    host_video_reset();
    host_media_reset();
    host_vorbis_reset();
    host_graphics_reset();
    host_d3d9_reset();
    host_d3dx_reset();
    uint32_t import = (address - IMPORT_THUNK_BASE) / 4;
    char message[1024];
    if (trap == TRAP_IMPORT && import < (uint32_t)host_import_count) {
        snprintf(message, sizeof message, "Startup checks passed (memory, files, TLS, synchronization, text, threads, window, input, PCM audio, Vorbis, music, GPU shader).\nWindows calls handled: %u\nNext Windows dependency:\n%s!%s",
                 handled_imports, host_imports[import].dll, host_imports[import].name);
    } else if (trap == TRAP_BUDGET) {
        snprintf(message,sizeof message,"Startup reached a probe execution limit at %#x after %llu ms.\nWindows calls handled: %u",address,(unsigned long long)elapsed_ms,handled_imports);
    } else if (trap == 14) {
        snprintf(message, sizeof message, "Graphics stopped at stage %u: %s", address, host_graphics_error());
    } else if (trap == TRAP_CANCELLED) {
        snprintf(message,sizeof message,"Startup stopped by request after %llu ms.\nWindows calls handled: %u",(unsigned long long)elapsed_ms,handled_imports);
    } else if (trap == TRAP_NOFN && !address) {
        snprintf(message,sizeof message,"Startup stopped: the game called a null function pointer. See the FNVCall diagnostics for its caller.");
    } else if (trap == TRAP_NOFN) {
        snprintf(message, sizeof message, "Startup checks passed (memory, files, TLS, synchronization, text, threads, window, input, PCM audio, Vorbis, music, GPU shader).\nWindows calls handled: %u\nUntranslated function: %#x.",
                 handled_imports, address);
    } else if (trap == TRAP_SOFTINT) {
        snprintf(message, sizeof message, "Guest software interrupt at %#x after %u Windows calls.",
                 address, handled_imports);
    } else if (trap == 10) {
        snprintf(message, sizeof message, "Guest requested ExitProcess(%u) after %u Windows calls.",
                 address, handled_imports);
    } else if (trap) {
        snprintf(message, sizeof message, "Startup stopped at %#x (trap %d).", address, trap);
    } else {
        snprintf(message, sizeof message, "Startup entry returned (EAX %#x).",
                 (uint32_t)LD(cpu.r, 4));
    }
    size_t used=strlen(message);
    snprintf(message+used,sizeof message-used,"\nRendered frames: %llu. Game draw calls: %llu.",(unsigned long long)frames,(unsigned long long)draws);
    return (*env)->NewStringUTF(env, message);
}

JNIEXPORT jstring JNICALL Java_org_fnvprobe_ProbeActivity_expectedExecutableDigest(JNIEnv *env, jclass cls) {
    (void)cls;return (*env)->NewStringUTF(env,STARTUP_SHA256);
}

extern void host_graphics_set_surface(ANativeWindow *window);
extern int host_graphics_surface_selftest(void);
JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_configureSurface(JNIEnv *env,jclass cls,jobject surface) {
    (void)cls;host_graphics_set_surface(surface?ANativeWindow_fromSurface(env,surface):NULL);
}
JNIEXPORT jstring JNICALL Java_org_fnvprobe_ProbeActivity_runSurfaceCheck(JNIEnv *env,jclass cls) {
    (void)cls;
    int failure=host_graphics_surface_selftest();
    host_d3d9_reset();
    if(!failure)return (*env)->NewStringUTF(env,"PASS: Android surface presentation.");
    char message[640];snprintf(message,sizeof message,"Surface check failed at stage %d: %s",failure,host_graphics_error());
    return (*env)->NewStringUTF(env,message);
}

JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_inputKey(JNIEnv *env,jclass cls,jint code,jboolean down) {
    (void)env;(void)cls;input_key(code,down);
}
JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_inputMotion(JNIEnv *env,jclass cls,jint dx,jint dy) {
    (void)env;(void)cls;input_mouse_move(dx,dy);
}
JNIEXPORT jboolean JNICALL Java_org_fnvprobe_ProbeActivity_gameMenuActive(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;return input_menu_active() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_org_fnvprobe_ProbeActivity_checkTouchAim(
        JNIEnv *env,jclass cls,jboolean fire,jboolean forward,jint expected_dx) {
    (void)env;(void)cls;
    uint8_t keys[256],buttons[8];int32_t dx,dy,dz;
    input_keyboard_state(keys);
    input_mouse_state(&dx,&dy,&dz,buttons);
    return buttons[1] && !!buttons[0] == !!fire && !!keys[0x11] == !!forward && dx == expected_dx
        ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_inputWheel(JNIEnv *env,jclass cls,jint delta) {
    (void)env;(void)cls;input_mouse_wheel(delta);
}
JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_inputButton(JNIEnv *env,jclass cls,jint button,jboolean down) {
    (void)env;(void)cls;input_mouse_button(button,down);
}

JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_inputGamepad(
        JNIEnv *env, jclass cls, jboolean connected, jint buttons, jint left_trigger, jint right_trigger,
        jint left_x, jint left_y, jint right_x, jint right_y) {
    (void)env; (void)cls;
    InputGamepad pad = {(uint16_t)buttons, (uint8_t)left_trigger, (uint8_t)right_trigger,
                        (int16_t)left_x, (int16_t)left_y, (int16_t)right_x, (int16_t)right_y};
    input_gamepad_update(connected, pad);
}

extern int host_xinput_selftest(int connected, int expected_right_x);
JNIEXPORT jboolean JNICALL Java_org_fnvprobe_ProbeActivity_runGamepadCheck(
        JNIEnv *env, jclass cls, jboolean connected, jint expected_right_x) {
    (void)env; (void)cls;
    return host_xinput_selftest(connected, expected_right_x) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jstring JNICALL Java_org_fnvprobe_ProbeActivity_runInputCheck(JNIEnv *env,jclass cls,jboolean verify) {
    (void)cls;int okay=1;
    if(verify){
        uint8_t keys[256],buttons[8];int32_t dx,dy,dz;input_keyboard_state(keys);input_mouse_state(&dx,&dy,&dz,buttons);
        uint32_t args[]={'W'};ShimFn query=host_find_shim("user32","GetAsyncKeyState");
        okay=keys[0x11]==0x80&&dx==7&&dy==-3&&dz==0&&buttons[0]==0x80&&query&&
            host_shim_test_call(query,args,1)==0x8001&&host_shim_test_call(query,args,1)==0x8000;
    }
    input_reset();host_win_reset();
    return (*env)->NewStringUTF(env,okay?"PASS: Android controls reached native keyboard/mouse state.":"Android input bridge check failed.");
}

JNIEXPORT jstring JNICALL Java_org_fnvprobe_ProbeActivity_runReleasedInputCheck(JNIEnv *env,jclass cls){
    (void)cls;uint8_t keys[256],buttons[8];int32_t dx,dy,dz;int okay=1;
    input_keyboard_state(keys);input_mouse_state(&dx,&dy,&dz,buttons);
    for(unsigned i=0;i<256;++i)if(keys[i])okay=0;
    for(unsigned i=0;i<8;++i)if(buttons[i])okay=0;
    return (*env)->NewStringUTF(env,okay?"PASS: Pausing released held game input.":"Held game input remained active after pause.");
}

extern void host_graphics_request_capture(void);
JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_requestTraceStop(JNIEnv *env,jclass cls){
    (void)env;(void)cls;rt_request_trace_stop();
}

JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_setPlayClosing(JNIEnv *env,jclass cls,jboolean closing){
    (void)env;(void)cls;
    atomic_store(&play_closing, closing);
    if (closing) rt_request_trace_stop();
}
static int pointer_range(uint32_t address){return address>=0x10000u&&address<0xfffffe00u;}
JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_reportGamePointer(JNIEnv *env,jclass cls){
    (void)env;(void)cls;
    if(!rt_mem())return;
    // Fixed layout of the SHA-256-pinned executable. Observation only; this
    // debug entry point exposes no arbitrary memory address or mutation.
    uint32_t manager=G32(0x11d8a80u);if(!pointer_range(manager))return;
    uint32_t cursor=G32(manager+0x28),tile=G32(manager+0xcc),menu=G32(manager+0xd0);
    uint32_t name=pointer_range(tile)?G32(tile+0x20):0;
    __android_log_print(ANDROID_LOG_INFO,"FNVPointer","manager=%#x cursor=%#x activeTile=%#x menu=%#x name=%.80s backbuffer=%ux%u viewport=%u,%u,%u,%u",manager,cursor,tile,menu,pointer_range(name)?GSTR(name):"",g_d3d.width,g_d3d.height,g_d3d.viewport[0],g_d3d.viewport[1],g_d3d.viewport[2],g_d3d.viewport[3]);
    float coords[4];memcpy(coords,MEM+manager+0x38,16);
    uint32_t rendered=G32(manager+0x16c);
    __android_log_print(ANDROID_LOG_INFO,"FNVPointer","mouse=%g,%g,%g,%g rendered=%#x renderedVtable=%#x",coords[0],coords[1],coords[2],coords[3],rendered,pointer_range(rendered)?G32(rendered):0);
    if(pointer_range(cursor)){
        uint32_t values=G32(cursor+0x14),count=G32(cursor+0x18);
        __android_log_print(ANDROID_LOG_INFO,"FNVPointer","cursor values=%#x count=%u raw=%#x,%#x,%#x,%#x",values,count,G32(cursor+0x10),G32(cursor+0x14),G32(cursor+0x18),G32(cursor+0x1c));
        if(pointer_range(values)&&count<=128)for(uint32_t i=0;i<count;++i){
            uint32_t value=G32(values+4*i);if(!pointer_range(value))continue;
            uint32_t id=G32(value);if(id==0xfa1||id==0xfa2){float number;memcpy(&number,MEM+value+8,4);__android_log_print(ANDROID_LOG_INFO,"FNVPointer","trait=%#x value=%g",id,number);}
        }
    }
}
JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_requestGameFrame(JNIEnv *env,jclass cls){
    (void)env;(void)cls;host_graphics_request_capture();
}


JNIEXPORT void JNICALL Java_org_fnvprobe_ProbeActivity_configureInputScript(JNIEnv *env,jclass cls,jboolean enabled){
    (void)env;(void)cls;atomic_store(&debug_input_script,enabled!=0);
}
