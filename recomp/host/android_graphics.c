#ifdef __ANDROID__
// Android OpenGL ES bridge. Guest resources retain the macOS runtime layout;
// D3D shader bytecode uses the shared decoder with a GLSL stage emitter.
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/performance_hint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "com.h"
#include "d3d9_backend.h"
#include "sm2msl.h"
#include "android_texture.h"
#include "input.h"

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLConfig config;
static unsigned width, height, pbuffer_width, pbuffer_height;
static pthread_mutex_t surface_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *pending_window, *active_window;
static int pending_window_width,pending_window_height;
static uint64_t pending_generation, active_generation = UINT64_MAX;
static int test_presentation;
static uint64_t previous_frame_draws;
static APerformanceHintSession *frame_hint;
static void (*close_frame_hint)(APerformanceHintSession *);
static int (*report_frame_hint)(APerformanceHintSession *,int64_t);
static int32_t frame_hint_thread;
static uint64_t previous_swap_ns;

static void reset_frame_hint(void) {
    if(frame_hint)close_frame_hint(frame_hint);
    frame_hint=NULL;
    frame_hint_thread=0;
    previous_swap_ns=0;
}

static void report_frame_work(void) {
    int32_t thread=gettid();
    if(thread!=frame_hint_thread) {
        reset_frame_hint();
        frame_hint_thread=thread;
        // The native service was added in Android 13. Resolve it at runtime.
        static void *android_library;
        if(!android_library)android_library=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
        if(!android_library)return;
        APerformanceHintManager *(*manager)(void)=dlsym(android_library,"APerformanceHint_getManager");
        APerformanceHintSession *(*create)(APerformanceHintManager *,const int32_t *,size_t,int64_t)
            =dlsym(android_library,"APerformanceHint_createSession");
        close_frame_hint=dlsym(android_library,"APerformanceHint_closeSession");
        report_frame_hint=dlsym(android_library,"APerformanceHint_reportActualWorkDuration");
        if(!manager||!create||!close_frame_hint||!report_frame_hint)return;
        APerformanceHintManager *service=manager();
        if(!service)return;
        // Leave a few milliseconds in the 30 FPS budget for final presentation.
        frame_hint=create(service,&thread,1,30000000);
        if(frame_hint)__android_log_print(ANDROID_LOG_INFO,"FNVGraphics","Android frame scheduling hint enabled");
    }
    if(frame_hint&&previous_swap_ns) {
        uint64_t elapsed=rt_rdtsc()-previous_swap_ns;
        if(elapsed>0&&elapsed<=250000000&&report_frame_hint(frame_hint,elapsed)) {
            close_frame_hint(frame_hint);
            frame_hint=NULL;
        }
    }
}
static uint32_t cached_render_states[256],cached_viewport[6],cached_scissor[4];
enum { INVALID_MASKS=1, INVALID_SCISSOR=2, INVALID_ALL=4 };
static unsigned render_state_invalid=INVALID_ALL;
static _Atomic int debug_capture_requested;
static _Atomic uint64_t window_frames;
void host_graphics_request_capture(void){atomic_store(&debug_capture_requested,1);}
uint64_t host_graphics_window_frames(void) {
    return atomic_load_explicit(&window_frames, memory_order_relaxed);
}
// Serialize imports while retaining EGL across consecutive graphics calls.
// Owners yield before nongraphics imports, at requested runtime polls, or on exit.
static pthread_mutex_t graphics_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t graphics_available=PTHREAD_COND_INITIALIZER;
static _Thread_local unsigned graphics_depth;
static _Thread_local int graphics_locked;
static _Thread_local int retained_on_thread;
static _Atomic int retain_context,context_handoff;
static pthread_t context_owner;
static int owner_valid;

static void release_context_locked(void) {
    if(display!=EGL_NO_DISPLAY&&context!=EGL_NO_CONTEXT&&eglGetCurrentContext()==context)
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    retained_on_thread=0;
    if(owner_valid&&pthread_equal(context_owner,pthread_self())) {
        owner_valid=0;
        atomic_store(&context_handoff,0);
        pthread_cond_broadcast(&graphics_available);
    }
}

void host_graphics_yield(void) {
    if(graphics_depth||!retained_on_thread)return;
    pthread_mutex_lock(&graphics_lock);
    release_context_locked();
    pthread_mutex_unlock(&graphics_lock);
}

void host_graphics_poll(void) {
    if(atomic_load(&context_handoff))host_graphics_yield();
}

void host_graphics_retain(int enabled) {
    atomic_store(&retain_context,enabled);
    if(!enabled)host_graphics_yield();
}

void host_graphics_enter(void) {
    // Retention already excludes other threads from the renderer. The owner
    // only takes the mutex again when it changes or relinquishes ownership.
    if(!graphics_depth&&!retained_on_thread) {
        pthread_mutex_lock(&graphics_lock);
        while(owner_valid&&!pthread_equal(context_owner,pthread_self())) {
            atomic_store(&context_handoff,1);
            pthread_cond_wait(&graphics_available,&graphics_lock);
        }
        graphics_locked=1;
    }
    ++graphics_depth;
}
void host_graphics_leave(void) {
    if(!graphics_depth)return;
    if(--graphics_depth)return;
    if(!graphics_locked) {
        if(atomic_load(&retain_context))return;
        host_graphics_yield();
        return;
    }
    if(atomic_load(&retain_context)&&context!=EGL_NO_CONTEXT&&
       (retained_on_thread||eglGetCurrentContext()==context)) {
        context_owner=pthread_self();
        owner_valid=1;
        retained_on_thread=1;
    } else release_context_locked();
    graphics_locked=0;
    pthread_mutex_unlock(&graphics_lock);
}
void host_graphics_unwind(void) {
    if(graphics_depth) {
        graphics_depth=0;
        if(!graphics_locked)pthread_mutex_lock(&graphics_lock);
        release_context_locked();
        graphics_locked=0;
        pthread_mutex_unlock(&graphics_lock);
    } else host_graphics_yield();
}

// Takes ownership of the reference returned by ANativeWindow_fromSurface.
void host_graphics_set_surface(ANativeWindow *window,int window_width,int window_height) {
    pthread_mutex_lock(&surface_lock);
    ANativeWindow *old = pending_window;
    pending_window=window;
    pending_window_width=window_width;
    pending_window_height=window_height;
    ++pending_generation;
    pthread_mutex_unlock(&surface_lock);
    if (old) ANativeWindow_release(old);
}
static char graphics_error[512];
static void reset_programs(void);
static void reset_textures(void);
static int texture_bridge_selftest(void);
static GLuint vao, scratch_vb, scratch_ib, constants[2], vertex_buffers[16];
static GLuint bound_vertex_buffer,bound_index_buffer;
static void bind_geometry_buffer(GLenum target,GLuint name) {
    GLuint *bound=target==GL_ARRAY_BUFFER?&bound_vertex_buffer:&bound_index_buffer;
    if(*bound==name)return;
    glBindBuffer(target,name);
    *bound=name;
}
static GLuint bound_read_fb,bound_draw_fb;
static void bind_framebuffer(GLenum target,GLuint framebuffer) {
    if ((target==GL_DRAW_FRAMEBUFFER||bound_read_fb==framebuffer) &&
        (target==GL_READ_FRAMEBUFFER||bound_draw_fb==framebuffer)) return;
    glBindFramebuffer(target,framebuffer);
    if(target!=GL_DRAW_FRAMEBUFFER)bound_read_fb=framebuffer;
    if(target!=GL_READ_FRAMEBUFFER)bound_draw_fb=framebuffer;
}
static GLuint active_program;
static void use_program(GLuint program) {
    if(active_program==program)return;
    glUseProgram(program);
    active_program=program;
}
static uint32_t geometry_epoch=1;
static size_t geometry_bytes;
// The two UBO bindings are shared by every linked program in this context.
static unsigned char constant_shadow[2][4416];
static unsigned constant_valid;
enum { CONSTANT_PAGES=4, CONSTANT_SLOTS_PER_PAGE=128 };
typedef struct {
    GLuint name;
    unsigned char *mapping;
    size_t stride;
    unsigned page, used;
    GLsync fences[CONSTANT_PAGES];
} ConstantStream;
static ConstantStream constant_streams[2];
static int constant_streams_checked;

static void reset_constant_streams(void) {
    for(unsigned slot=0;slot<2;++slot) {
        ConstantStream *stream=&constant_streams[slot];
        if(context==EGL_NO_CONTEXT)continue;
        for(unsigned page=0;page<CONSTANT_PAGES;++page)
            if(stream->fences[page])glDeleteSync(stream->fences[page]);
        if(stream->name)glDeleteBuffers(1,&stream->name);
    }
    memset(constant_streams,0,sizeof constant_streams);
    constant_streams_checked=0;
}

static void init_constant_streams(void) {
    constant_streams_checked=1;
    GLint count=0;
    glGetIntegerv(GL_NUM_EXTENSIONS,&count);
    for(GLint i=0;i<count;++i) {
        const char *extension=(const char *)glGetStringi(GL_EXTENSIONS,i);
        if(!extension||strcmp(extension,"GL_EXT_buffer_storage"))continue;
        PFNGLBUFFERSTORAGEEXTPROC storage=(PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT");
        if(!storage)return;
        GLint alignment=0;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT,&alignment);
        if(alignment<=0||alignment>4096)return;
        GLbitfield flags=GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT_EXT|GL_MAP_COHERENT_BIT_EXT;
        for(unsigned slot=0;slot<2;++slot) {
            ConstantStream *stream=&constant_streams[slot];
            size_t block=sizeof constant_shadow[slot];
            stream->stride=(block+alignment-1)/alignment*alignment;
            size_t size=stream->stride*CONSTANT_SLOTS_PER_PAGE*CONSTANT_PAGES;
            glGenBuffers(1,&stream->name);
            glBindBuffer(GL_UNIFORM_BUFFER,stream->name);
            storage(GL_UNIFORM_BUFFER,size,NULL,flags);
            if(glGetError()==GL_NO_ERROR)
                stream->mapping=glMapBufferRange(GL_UNIFORM_BUFFER,0,size,flags);
            if(!stream->mapping) {
                glGetError();
                reset_constant_streams();
                constant_streams_checked=1;
                __android_log_print(ANDROID_LOG_WARN,"FNVGraphics","Persistent shader constants unavailable; using ordinary uploads");
                return;
            }
        }
        __android_log_print(ANDROID_LOG_INFO,"FNVGraphics","Persistent shader constants enabled");
        return;
    }
}

static int stream_constant_block(unsigned slot,const void *bytes,size_t size) {
    if(!constant_streams_checked)init_constant_streams();
    ConstantStream *stream=&constant_streams[slot];
    if(!stream->mapping)return 0;
    if(stream->used==CONSTANT_SLOTS_PER_PAGE) {
        unsigned next=(stream->page+1)%CONSTANT_PAGES;
        if(stream->fences[next]) {
            GLenum status=glClientWaitSync(stream->fences[next],0,0);
            if(status!=GL_ALREADY_SIGNALED&&status!=GL_CONDITION_SATISFIED)return 0;
            glDeleteSync(stream->fences[next]);
            stream->fences[next]=NULL;
        }
        // Each stage retires its own page only when a new range replaces its binding.
        // If the GPU is behind, leave mapped pages untouched and use the ordinary buffer.
        stream->fences[stream->page]=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        if(!stream->fences[stream->page])return 0;
        stream->page=next;
        stream->used=0;
    }
    size_t offset=(stream->page*CONSTANT_SLOTS_PER_PAGE+stream->used++)*stream->stride;
    memcpy(stream->mapping+offset,bytes,size);
    glBindBufferRange(GL_UNIFORM_BUFFER,slot,stream->name,offset,size);
    return 1;
}
typedef struct {
    GLuint buffer;
    GLenum type;
    GLint components;
    GLsizei stride;
    uintptr_t offset;
    GLboolean normalized;
    int valid;
} GLVertexAttribute;
static GLVertexAttribute attribute_cache[16];
static unsigned enabled_attributes;
static int attribute_state_known;
const char *host_graphics_error(void) { return graphics_error; }
static void graphics_fail(const char *message, uint32_t stage) {
    snprintf(graphics_error, sizeof graphics_error, "%s", message);
    __android_log_print(ANDROID_LOG_ERROR, "FNVGraphics", "%s", message);
    rt_trap(NULL, 14, stage);
}
static int open_context(unsigned w, unsigned h) {
    if (context != EGL_NO_CONTEXT) return eglMakeCurrent(display,surface,surface,context)==EGL_TRUE;
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor, count;
    const EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT|EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_NONE};
    const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    if (display == EGL_NO_DISPLAY || !eglInitialize(display,&major,&minor) ||
        !eglChooseConfig(display,attrs,&config,1,&count) || !count) return 0;
    eglBindAPI(EGL_OPENGL_ES_API);
    context = eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
    const EGLint pb[] = {EGL_WIDTH,(EGLint)w,EGL_HEIGHT,(EGLint)h,EGL_NONE};
    surface = eglCreatePbufferSurface(display,config,pb);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display,surface,surface,context)) return 0;
    width=pbuffer_width=w; height=pbuffer_height=h;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&scratch_vb); glGenBuffers(1,&scratch_ib); glGenBuffers(2,constants); glGenBuffers(16,vertex_buffers);
    __android_log_print(ANDROID_LOG_INFO,"FNVGraphics","GPU: %s; %s",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    return glGetError()==GL_NO_ERROR;
}
typedef struct VisibilitySample {
    GLuint name, result;
    unsigned references;
    int ready;
} VisibilitySample;
typedef struct VisibilityEntry {
    VisibilitySample *sample;
    struct VisibilityEntry *next;
} VisibilityEntry;
typedef struct EventQuery {
    uint32_t object;
    GLsync fence;
    GLuint samples;
    int building, cached;
    uint32_t result;
    VisibilityEntry *visibility;
    struct EventQuery *next;
} EventQuery;
static GLuint sample_counter;
static unsigned occlusion_active;
static GLuint depth_snapshot,depth_snapshot_fb;
static uint32_t depth_snapshot_source;
static unsigned depth_snapshot_width,depth_snapshot_height;
static GLenum depth_snapshot_format;
static uint64_t depth_generation,depth_snapshot_generation=UINT64_MAX;
static EventQuery *event_queries;
static int reported_visibility;

static void clear_visibility(EventQuery *query) {
    while (query->visibility) {
        VisibilityEntry *entry = query->visibility;
        VisibilitySample *sample = entry->sample;
        query->visibility = entry->next;
        if (!--sample->references) {
            if (sample->name && context != EGL_NO_CONTEXT) glDeleteQueries(1, &sample->name);
            free(sample);
        }
        free(entry);
    }
}

static void begin_visibility_draw(void) {
    VisibilitySample *sample = calloc(1, sizeof *sample);
    if (!sample) graphics_fail("Unable to allocate visibility sample.",74);
    for (EventQuery *query = event_queries; query; query = query->next) {
        if (!query->building) continue;
        VisibilityEntry *entry = malloc(sizeof *entry);
        if (!entry) {
            if (!sample->references) free(sample);
            graphics_fail("Unable to track visibility query.",74);
        }
        *entry = (VisibilityEntry){sample, query->visibility};
        query->visibility = entry;
        ++sample->references;
    }
    if (!sample->references) {
        free(sample);
        graphics_fail("Visibility draw has no active query.",74);
    }
    glGenQueries(1, &sample->name);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, sample->name);
    if (!reported_visibility) {
        __android_log_print(ANDROID_LOG_INFO,"FNVGraphics","Using hardware visibility for a late depth/stencil query draw.");
        reported_visibility = 1;
    }
}
unsigned d3d_backend_event_count(void){unsigned count=0;for(EventQuery *q=event_queries;q;q=q->next)++count;return count;}
static EventQuery *event_query(uint32_t object){for(EventQuery *q=event_queries;q;q=q->next)if(q->object==object)return q;return NULL;}
static void event_context(D3DState *state){
    if(context==EGL_NO_CONTEXT&&!open_context(state&&state->width?state->width:16,state&&state->height?state->height:16))graphics_fail("Unable to initialize GPU event context.",70);
    if(eglGetCurrentContext()!=context&&!eglMakeCurrent(display,surface,surface,context))graphics_fail("Unable to acquire GPU event context.",70);
}
uint32_t d3d_backend_event_issue(D3DState *state,uint32_t object){
    event_context(state);EventQuery *q=event_query(object);
    if(!q){q=calloc(1,sizeof *q);if(!q)return 0x8007000eu;q->object=object;q->next=event_queries;event_queries=q;}
    GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    if(!fence||glGetError()!=GL_NO_ERROR)graphics_fail("GPU event fence creation failed.",71);
    if(q->fence)glDeleteSync(q->fence);q->fence=fence;return 0;
}
uint32_t d3d_backend_event_poll(uint32_t object,int flush){
    EventQuery *q=event_query(object);if(!q||!q->fence)return 0; // Newly created queries are signaled.
    event_context(NULL);
    GLenum result=glClientWaitSync(q->fence,flush?GL_SYNC_FLUSH_COMMANDS_BIT:0,0);
    if(result==GL_ALREADY_SIGNALED||result==GL_CONDITION_SATISFIED)return 0;
    if(result==GL_TIMEOUT_EXPIRED)return 1;
    graphics_fail("GPU event polling failed.",72);return 0x88760868u;
}
static void sample_snapshot(EventQuery *q,unsigned offset){
    glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT|GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER,sample_counter);glBindBuffer(GL_COPY_WRITE_BUFFER,q->samples);
    glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,0,offset,4);
    glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT|GL_BUFFER_UPDATE_BARRIER_BIT);
}
uint32_t d3d_backend_occlusion_issue(D3DState *state,uint32_t object,uint32_t flags){
    event_context(state);EventQuery *q=event_query(object);
    if(flags==2){
        if(q&&q->building)return 0x8876086cu;
        if(!q){q=calloc(1,sizeof *q);if(!q)return 0x8007000eu;q->object=object;q->next=event_queries;event_queries=q;}
        if(!sample_counter){
            GLint major=0,minor=0;glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);
            if(major<3||(major==3&&minor<1))graphics_fail("Occlusion sample counting requires OpenGL ES 3.1.",73);
            uint32_t zero=0;glGenBuffers(1,&sample_counter);glBindBuffer(GL_ATOMIC_COUNTER_BUFFER,sample_counter);
            glBufferData(GL_ATOMIC_COUNTER_BUFFER,4,&zero,GL_DYNAMIC_COPY);
        }
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER,0,sample_counter);
        if(!q->samples){uint32_t zero[2]={0};glGenBuffers(1,&q->samples);glBindBuffer(GL_COPY_WRITE_BUFFER,q->samples);glBufferData(GL_COPY_WRITE_BUFFER,8,zero,GL_DYNAMIC_COPY);}
        if(q->fence){glDeleteSync(q->fence);q->fence=NULL;}
        clear_visibility(q);
        q->cached=0;sample_snapshot(q,0);q->building=1;++occlusion_active;
    }else if(flags==1){
        if(!q||!q->building)return 0x8876086cu;
        sample_snapshot(q,4);q->building=0;--occlusion_active;
        q->fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        if(!q->fence)graphics_fail("Occlusion sample fence creation failed.",74);
    }else return 0x8876086cu;
    if(glGetError()!=GL_NO_ERROR)graphics_fail("Occlusion sample snapshot failed.",74);
    return 0;
}
uint32_t d3d_backend_occlusion_poll(uint32_t object,int flush,uint32_t *samples){
    EventQuery *q=event_query(object);if(q&&q->building)return 0x8876086cu;
    uint32_t status=d3d_backend_event_poll(object,flush);if(status)return status;
    if(q&&q->samples&&!q->cached){
        glBindBuffer(GL_COPY_READ_BUFFER,q->samples);
        const uint32_t *data=glMapBufferRange(GL_COPY_READ_BUFFER,0,8,GL_MAP_READ_BIT);
        if(!data)graphics_fail("Occlusion sample readback failed.",75);
        q->result=data[1]-data[0];
        if(!glUnmapBuffer(GL_COPY_READ_BUFFER))graphics_fail("Occlusion sample readback became invalid.",75);
        for (VisibilityEntry *entry = q->visibility; entry; entry = entry->next) {
            VisibilitySample *sample = entry->sample;
            if (!sample->ready) {
                glGetQueryObjectuiv(sample->name, GL_QUERY_RESULT, &sample->result);
                sample->ready = 1;
            }
            // GLES reports visibility for these draws, so add one for each visible draw.
            q->result += sample->result != 0;
        }
        clear_visibility(q);
        q->cached=1;
    }
    if(samples)*samples=q?q->result:0;
    return 0;
}
void d3d_backend_event_release(uint32_t object){
    EventQuery **link=&event_queries;
    while(*link){EventQuery *q=*link;if(q->object==object){
        if(q->fence&&context!=EGL_NO_CONTEXT){event_context(NULL);glDeleteSync(q->fence);}
        if(q->building&&occlusion_active)--occlusion_active;
        if(q->samples&&context!=EGL_NO_CONTEXT){event_context(NULL);glDeleteBuffers(1,&q->samples);}
        clear_visibility(q);
        *link=q->next;free(q);return;
    }link=&q->next;}
}
static void reset_event_queries(void){
    while(event_queries)d3d_backend_event_release(event_queries->object);
    if(context!=EGL_NO_CONTEXT){if(sample_counter)glDeleteBuffers(1,&sample_counter);if(depth_snapshot)glDeleteTextures(1,&depth_snapshot);if(depth_snapshot_fb)glDeleteFramebuffers(1,&depth_snapshot_fb);}
    sample_counter=depth_snapshot=depth_snapshot_fb=0;occlusion_active=0;depth_generation=0;depth_snapshot_generation=UINT64_MAX;depth_snapshot_source=0;
    reported_visibility=0;
}
static void graphics_reset_impl(void) {
    reset_frame_hint();
    if(context!=EGL_NO_CONTEXT)eglMakeCurrent(display,surface,surface,context);
    reset_programs();
    reset_textures();
    reset_event_queries();
    reset_constant_streams();
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display,surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display,context);
        eglTerminate(display);
    }
    if(active_window){ANativeWindow_release(active_window);active_window=NULL;}
    active_generation=UINT64_MAX;
    display=EGL_NO_DISPLAY;context=EGL_NO_CONTEXT;surface=EGL_NO_SURFACE;
    owner_valid=0;
    retained_on_thread=0;
    atomic_store(&context_handoff,0);
    pthread_cond_broadcast(&graphics_available);
    bound_read_fb=bound_draw_fb=0;
    bound_vertex_buffer=bound_index_buffer=0;
    constant_valid=0;
    render_state_invalid=INVALID_ALL;
    memset(attribute_cache,0,sizeof attribute_cache);
    enabled_attributes=0;
    attribute_state_known=0;
    ++geometry_epoch;
    geometry_bytes=0;
    vao=scratch_vb=scratch_ib=constants[0]=constants[1]=0; memset(vertex_buffers,0,sizeof vertex_buffers); width=height=0;
}
void host_graphics_reset(void) {
    host_graphics_enter();
    if(!graphics_locked) {
        pthread_mutex_lock(&graphics_lock);
        graphics_locked=1;
    }
    graphics_reset_impl();
    previous_frame_draws=0;
    host_graphics_leave();
}
// Surface lifecycle calls only publish a window. EGL remains on the guest's
// rendering thread, including destruction and replacement of old surfaces.
static int refresh_surface(unsigned game_width,unsigned game_height) {
    pthread_mutex_lock(&surface_lock);
    uint64_t generation=pending_generation;
    ANativeWindow *next=pending_window;
    int window_width=pending_window_width,window_height=pending_window_height;
    if(next)ANativeWindow_acquire(next);
    pthread_mutex_unlock(&surface_lock);
    if(generation==active_generation){if(next)ANativeWindow_release(next);return 1;}
    reset_frame_hint();
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);
    if(active_window)ANativeWindow_release(active_window);
    active_window=next;active_generation=generation;
    if(next) {
        EGLint visual;eglGetConfigAttrib(display,config,EGL_NATIVE_VISUAL_ID,&visual);
        int buffer_width=0,buffer_height=0;
        if(window_width>0&&window_height>0&&game_width&&game_height) {
            // Keep the window aspect, including bars, and let Android scale the
            // finished buffer. Avoid drawing more pixels than the window needs.
            double scale=fmax((double)game_width/window_width,(double)game_height/window_height);
            if(scale<1.0) {
                buffer_width=(int)lround(window_width*scale);
                buffer_height=(int)lround(window_height*scale);
            }
        }
        if(ANativeWindow_setBuffersGeometry(next,buffer_width,buffer_height,visual)&&
           ANativeWindow_setBuffersGeometry(next,0,0,visual))return 0;
        surface=eglCreateWindowSurface(display,config,next,NULL);
    } else {
        EGLint attributes[]={EGL_WIDTH,(EGLint)pbuffer_width,EGL_HEIGHT,(EGLint)pbuffer_height,EGL_NONE};
        surface=eglCreatePbufferSurface(display,config,attributes);
    }
    if(surface==EGL_NO_SURFACE||!eglMakeCurrent(display,surface,surface,context))return 0;
    EGLint w,h;eglQuerySurface(display,surface,EGL_WIDTH,&w);eglQuerySurface(display,surface,EGL_HEIGHT,&h);
    if(next&&window_width>0&&window_height>0)
        __android_log_print(ANDROID_LOG_INFO,"FNVGraphics","Presentation buffer %dx%d for window %dx%d",w,h,window_width,window_height);
    width=w;height=h;return w>0&&h>0;
}
static GLuint compile_shader(GLenum stage, const char *source, char *error, size_t size) {
    GLuint shader=glCreateShader(stage); glShaderSource(shader,1,&source,NULL); glCompileShader(shader);
    GLint okay;glGetShaderiv(shader,GL_COMPILE_STATUS,&okay);
    if(!okay){
        glGetShaderInfoLog(shader,(GLsizei)size,NULL,error);
        const char *root=getenv("FNV_ROOT");
        if(root){char path[2048];snprintf(path,sizeof path,"%s/.probe-failed-%s.glsl",root,stage==GL_VERTEX_SHADER?"vertex":"pixel");
            FILE *file=fopen(path,"w");if(file){fwrite(source,1,strlen(source),file);fclose(file);}}
        glDeleteShader(shader);return 0;
    }return shader;
}
static GLuint link_program(GLuint vs, GLuint ps, char *error, size_t size) {
    GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,ps);glLinkProgram(p);
    GLint okay;glGetProgramiv(p,GL_LINK_STATUS,&okay);
    if(!okay){glGetProgramInfoLog(p,(GLsizei)size,NULL,error);glDeleteProgram(p);return 0;}return p;
}
static int graphics_selftest_impl(void) {
    graphics_error[0]=0;
    if(android_texture_selftest())return 4;
    if(!open_context(32,32)){host_graphics_reset();return 1;}
    // vs_3_0: dcl_position v0; dcl_position o0; mov o0,v0.
    const uint32_t vs[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,
        0x0200001f,0x80000000,0xe00f0000,0x02000001,0xe00f0000,0x90e40000,0xffff};
    // ps_3_0: def c0,1,0,0,1; mov oC0,c0.
    const uint32_t ps[]={0xffff0300,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,
        0x02000001,0x800f0800,0xa0e40000,0xffff};
    SMInfo vi={0},pi={0};char error[512]={0};
    char *v=sm_translate_glsl(vs,sizeof vs/4,&vi,NULL,0,error,sizeof error);
    char *p=sm_translate_glsl(ps,sizeof ps/4,&pi,NULL,0,error,sizeof error);
    GLuint sv=v?compile_shader(GL_VERTEX_SHADER,v,error,sizeof error):0;
    GLuint sp=p?compile_shader(GL_FRAGMENT_SHADER,p,error,sizeof error):0;
    free(v);free(p);
    GLuint program=sv&&sp?link_program(sv,sp,error,sizeof error):0;
    if(sv)glDeleteShader(sv);if(sp)glDeleteShader(sp);
    if(!program){snprintf(graphics_error,sizeof graphics_error,"Shader fixture: %s",error);host_graphics_reset();return 2;}
    use_program(program);glUniform1i(glGetUniformLocation(program,"alpha_func"),8);
    GLfloat triangle[]={-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    bind_geometry_buffer(GL_ARRAY_BUFFER,scratch_vb);glBufferData(GL_ARRAY_BUFFER,sizeof triangle,triangle,GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0);
    attribute_state_known=0;
    render_state_invalid=INVALID_ALL;
    glViewport(0,0,32,32);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char pixel[4]={0};glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    GLenum result=glGetError();glDeleteProgram(program);
    int okay=result==GL_NO_ERROR&&pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255;
    int bridge=okay?texture_bridge_selftest():0;
    host_graphics_reset();return !okay?3:bridge?10+bridge:0;
}

#define OF(o,i) ((o)+16+4*(i))
// Programs key on immutable guest shader objects, which live until device reset.
typedef struct Program {
    uint32_t vs,ps,active_samplers;
    unsigned tests,constant_blocks;
    GLuint name;
    int counting,manual_depth,visibility_only,uniforms_valid;
    GLint color_mask,halfpixel,clip_enable,clip_planes,alpha_func,alpha_ref;
    GLint depth_func,depth_scale;
    uint32_t last_viewport[2],last_clip_enable,last_alpha_func,last_alpha_ref,last_color_mask;
    float last_clip_planes[D3D_USER_CLIP_PLANES][4];
    SMInfo vi,pi;
    struct Program *next;
} Program;
#include "android_graphics_texture.inc"
enum { PROGRAM_BUCKETS=256 };
static Program *programs[PROGRAM_BUCKETS];
int host_graphics_selftest(void) {
    host_graphics_enter();int result=graphics_selftest_impl();host_graphics_leave();return result;
}
static void reset_programs(void) {
    for(unsigned i=0;i<PROGRAM_BUCKETS;++i){
        while(programs[i]){
            Program *p=programs[i];programs[i]=p->next;
            if(context!=EGL_NO_CONTEXT)glDeleteProgram(p->name);
            free(p);
        }
    }
    active_program=0;
}
enum { QUERY_ACTIVE=1, QUERY_WRITES=2, QUERY_STENCIL=4 };
static int query_draw_mode(const D3DState *s) {
    if (!occlusion_active) return 0;
    int stencil = s->ds && s->rs[52];
    int stencil_write = stencil && s->rs[59] &&
        ((s->rs[53]>1 || s->rs[54]>1 || s->rs[55]>1) ||
         (s->rs[185] && (s->rs[186]>1 || s->rs[187]>1 || s->rs[188]>1)));
    int writes = (s->ds && s->rs[7] && s->rs[14]) || stencil_write;
    return QUERY_ACTIVE | (writes ? QUERY_WRITES : 0) | (stencil ? QUERY_STENCIL : 0);
}

static Program *program_for(D3DState *s) {
    int counting=query_draw_mode(s);
    unsigned tests=0;
    if(s->rs[152]&((1u<<D3D_USER_CLIP_PLANES)-1u))tests|=SM_GLSL_CLIP_PLANES;
    if(s->rs[15]&&s->rs[25]&&s->rs[25]!=8)tests|=SM_GLSL_ALPHA_TEST;
    // Guest objects are aligned. Mix their upper address bits before bucketing.
    uint32_t hash=(s->vs>>4)*0x9e3779b1u^(s->ps>>4)*0x85ebca77u;
    hash^=(unsigned)counting*0xc2b2ae3du^tests*0x27d4eb2fu;
    unsigned bucket=(hash^(hash>>16))&(PROGRAM_BUCKETS-1);
    for(Program *p=programs[bucket];p;p=p->next)
        if(p->vs==s->vs&&p->ps==s->ps&&p->counting==counting&&p->tests==tests)return p;
    if(!s->vs||!s->ps)graphics_fail("Fixed-function shader path has not been ported yet.",20);
    Program *p=calloc(1,sizeof *p);if(!p)graphics_fail("Unable to allocate shader state.",21);
    p->vs=s->vs;p->ps=s->ps;p->counting=counting;p->tests=tests;char error[512]={0};
    uint32_t vc=G32(OF(s->vs,0)),vn=G32(OF(s->vs,1)),pc=G32(OF(s->ps,0)),pn=G32(OF(s->ps,1));
    if(!sm_reflect((uint32_t*)(MEM+pc),pn,&p->pi,error,sizeof error)){free(p);graphics_fail(error,22);}
    // Early tests can write depth/stencil before discard. Keep the original
    // late-test shader for those draws and use GLES hardware visibility.
    p->visibility_only = ((counting & QUERY_WRITES) && (tests || p->pi.may_discard || p->pi.writes_depth))
        || ((counting & QUERY_STENCIL) && p->pi.writes_depth);
    p->manual_depth=counting&&!p->visibility_only&&p->pi.writes_depth;
    int query_mode=counting&&!p->visibility_only?(p->manual_depth?2:1):0;
    if(p->manual_depth){GLint maximum=0;glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&maximum);if(__builtin_popcount(p->pi.samplers)+1>maximum)graphics_fail("Depth-output occlusion query requires an additional fragment sampler.",76);}
    char *ps=sm_translate_glsl_query((uint32_t*)(MEM+pc),pn,&p->pi,NULL,0,error,sizeof error,query_mode,tests);
    char *vs=sm_translate_glsl_query((uint32_t*)(MEM+vc),vn,&p->vi,p->pi.io_keys,p->pi.n_io,error,sizeof error,query_mode,tests);
    GLuint v=vs?compile_shader(GL_VERTEX_SHADER,vs,error,sizeof error):0;
    GLuint f=ps?compile_shader(GL_FRAGMENT_SHADER,ps,error,sizeof error):0;
    free(vs);free(ps);p->name=v&&f?link_program(v,f,error,sizeof error):0;
    if(v)glDeleteShader(v);if(f)glDeleteShader(f);
    if(!p->name){free(p);graphics_fail(error,23);}
    p->color_mask=glGetUniformLocation(p->name,"d3dcolor_mask");
    p->halfpixel=glGetUniformLocation(p->name,"halfpixel");
    p->clip_enable=glGetUniformLocation(p->name,"clip_enable");
    p->clip_planes=glGetUniformLocation(p->name,"clip_planes[0]");
    p->alpha_func=glGetUniformLocation(p->name,"alpha_func");
    p->alpha_ref=glGetUniformLocation(p->name,"alpha_ref");
    use_program(p->name);
    for(unsigned slot=0;slot<20;++slot){
        unsigned i=slot<16?slot:slot-16;
        SMInfo *info=slot<16?&p->pi:&p->vi;
        if(!(info->samplers&(1u<<i)))continue;
        char name[24];snprintf(name,sizeof name,"%s_t%u",slot<16?"ps":"vs",i);
        GLint location=glGetUniformLocation(p->name,name);
        if(location<0)continue;
        p->active_samplers|=1u<<slot;
        glUniform1i(location,slot);
    }
    if(p->manual_depth){
        p->depth_func=glGetUniformLocation(p->name,"fnv_depth_func");
        p->depth_scale=glGetUniformLocation(p->name,"fnv_depth_scale");
        glUniform1i(glGetUniformLocation(p->name,"fnv_query_depth"),20);
    }
    GLuint block=glGetUniformBlockIndex(p->name,"VertexConstants");
    if(block!=GL_INVALID_INDEX){glUniformBlockBinding(p->name,block,0);p->constant_blocks|=1;}
    block=glGetUniformBlockIndex(p->name,"PixelConstants");
    if(block!=GL_INVALID_INDEX){glUniformBlockBinding(p->name,block,1);p->constant_blocks|=2;}
    p->next=programs[bucket];programs[bucket]=p;return p;
}
static void upload_constant_block(unsigned slot,const void *floats,size_t fsize,
                                  const void *integers,size_t isize,const void *booleans,size_t bsize) {
    unsigned char *shadow=constant_shadow[slot];
    if((constant_valid&(1u<<slot))&&!memcmp(shadow,floats,fsize)&&
       !memcmp(shadow+fsize,integers,isize)&&!memcmp(shadow+fsize+isize,booleans,bsize))return;
    memcpy(shadow,floats,fsize);memcpy(shadow+fsize,integers,isize);
    memcpy(shadow+fsize+isize,booleans,bsize);
    size_t size=fsize+isize+bsize;
    if(!stream_constant_block(slot,shadow,size)) {
        glBindBuffer(GL_UNIFORM_BUFFER,constants[slot]);
        glBufferData(GL_UNIFORM_BUFFER,size,shadow,GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER,slot,constants[slot]);
    }
    constant_valid|=1u<<slot;
}
static void upload_constants(D3DState *s,Program *p) {
    if(p->constant_blocks&1)upload_constant_block(0,s->vsc,sizeof s->vsc,s->vsi,sizeof s->vsi,s->vsb,sizeof s->vsb);
    if(p->constant_blocks&2)upload_constant_block(1,s->psc,sizeof s->psc,s->psi,sizeof s->psi,s->psb,sizeof s->psb);
    // Convert D3D9 pixel centres using the current viewport, including offscreen targets.
    int first=!p->uniforms_valid;
    uint32_t w=s->viewport[2],h=s->viewport[3];
    if(first||w!=p->last_viewport[0]||h!=p->last_viewport[1]){
        glUniform2f(p->halfpixel,w?1.0f/w:0.0f,h?-1.0f/h:0.0f);
        p->last_viewport[0]=w;p->last_viewport[1]=h;
    }
    uint32_t clip=s->rs[152]&((1u<<D3D_USER_CLIP_PLANES)-1u);
    if(first||clip!=p->last_clip_enable){glUniform1ui(p->clip_enable,clip);p->last_clip_enable=clip;}
    if(first||memcmp(p->last_clip_planes,s->clip_planes,sizeof p->last_clip_planes)){
        glUniform4fv(p->clip_planes,D3D_USER_CLIP_PLANES,&s->clip_planes[0][0]);
        memcpy(p->last_clip_planes,s->clip_planes,sizeof p->last_clip_planes);
    }
    uint32_t func=s->rs[15]?s->rs[25]:8,ref=s->rs[24]&255;
    if(first||func!=p->last_alpha_func){glUniform1i(p->alpha_func,func);p->last_alpha_func=func;}
    if(first||ref!=p->last_alpha_ref){glUniform1f(p->alpha_ref,ref/255.0f);p->last_alpha_ref=ref;}
    p->uniforms_valid=1;
}

static void target(D3DState *s) { bind_target(s); }

static GLenum primitive(uint32_t p,uint32_t count,GLsizei *vertices) {
    switch(p){case 1:*vertices=count;return GL_POINTS;case 2:*vertices=count*2;return GL_LINES;
    case 3:*vertices=count+1;return GL_LINE_STRIP;case 4:*vertices=count*3;return GL_TRIANGLES;
    case 5:*vertices=count+2;return GL_TRIANGLE_STRIP;case 6:*vertices=count+2;return GL_TRIANGLE_FAN;}
    graphics_fail("Unsupported primitive topology.",4);return GL_TRIANGLES;
}
static GLenum comparison(uint32_t value){static const GLenum values[]={GL_ALWAYS,GL_NEVER,GL_LESS,GL_EQUAL,GL_LEQUAL,GL_GREATER,GL_NOTEQUAL,GL_GEQUAL,GL_ALWAYS};return value<=8?values[value]:GL_ALWAYS;}
static GLenum blend(uint32_t value) {
    static const GLenum values[]={GL_ONE,GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,
        GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,GL_DST_COLOR,GL_ONE_MINUS_DST_COLOR,GL_SRC_ALPHA_SATURATE};
    if(value==14)return GL_CONSTANT_COLOR;if(value==15)return GL_ONE_MINUS_CONSTANT_COLOR;
    if(value>11)graphics_fail("Unsupported blend factor.",5);return values[value];
}
static GLenum blend_operation(uint32_t op) {
    switch(op){case 0:case 1:return GL_FUNC_ADD;case 2:return GL_FUNC_SUBTRACT;case 3:return GL_FUNC_REVERSE_SUBTRACT;case 4:return GL_MIN;case 5:return GL_MAX;}
    graphics_fail("Unsupported blend operation.",60);return GL_FUNC_ADD;
}
static GLenum stencil_operation(uint32_t op) {
    static const GLenum values[]={GL_KEEP,GL_KEEP,GL_ZERO,GL_REPLACE,GL_INCR,GL_DECR,GL_INVERT,GL_INCR_WRAP,GL_DECR_WRAP};
    if(op>8)graphics_fail("Unsupported stencil operation.",61);return values[op];
}
static void state(D3DState *s) {
    unsigned invalid=render_state_invalid;
    int first=(invalid&INVALID_ALL)!=0;
#define CHANGED(i) (first || cached_render_states[i]!=s->rs[i])
    if(first||memcmp(cached_viewport,s->viewport,4*sizeof(uint32_t)))
        glViewport(s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3]);
    if(first||memcmp(cached_viewport+4,s->viewport+4,2*sizeof(uint32_t))){
        float lo,hi;memcpy(&lo,s->viewport+4,4);memcpy(&hi,s->viewport+5,4);glDepthRangef(lo,hi);
    }
    if(CHANGED(7)){if(s->rs[7])glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);}
    if(CHANGED(14)||(invalid&INVALID_MASKS))glDepthMask(s->rs[14]!=0);
    if(CHANGED(23))glDepthFunc(comparison(s->rs[23]));
    if(s->rs[27]&&(CHANGED(27)||CHANGED(19)||CHANGED(20)||CHANGED(193)||
        CHANGED(171)||CHANGED(206)||CHANGED(207)||CHANGED(208)||CHANGED(209))) {
        glEnable(GL_BLEND);uint32_t src=s->rs[19],dst=s->rs[20];
        if(src==12){src=5;dst=6;}else if(src==13){src=6;dst=5;}
        uint32_t factor=s->rs[193];glBlendColor(((factor>>16)&255)/255.f,((factor>>8)&255)/255.f,(factor&255)/255.f,(factor>>24)/255.f);
        glBlendFuncSeparate(blend(src),blend(dst),blend(s->rs[206]?s->rs[207]:src),blend(s->rs[206]?s->rs[208]:dst));
        glBlendEquationSeparate(blend_operation(s->rs[171]),blend_operation(s->rs[206]?s->rs[209]:s->rs[171]));
    } else if(CHANGED(27)&&!s->rs[27])glDisable(GL_BLEND);
    if(CHANGED(168)||(invalid&INVALID_MASKS)){
        uint32_t mask=s->rs[168];glColorMask(mask&1,mask&2,mask&4,mask&8);
    }
    if(CHANGED(22)){
        if(s->rs[22]==2||s->rs[22]==3){glEnable(GL_CULL_FACE);glCullFace(s->rs[22]==2?GL_FRONT:GL_BACK);}
        else glDisable(GL_CULL_FACE);
    }
    if(first)glFrontFace(GL_CCW);
    if(CHANGED(174)||(invalid&INVALID_SCISSOR)||(s->rs[174]&&memcmp(cached_scissor,s->scissor,sizeof cached_scissor))){
        if(s->rs[174]){glEnable(GL_SCISSOR_TEST);glScissor(s->scissor[0],s->scissor[1],s->scissor[2]-s->scissor[0],s->scissor[3]-s->scissor[1]);}
        else glDisable(GL_SCISSOR_TEST);
    }
    if(s->rs[52]&&(CHANGED(52)||CHANGED(59)||(invalid&INVALID_MASKS)||CHANGED(56)||
        CHANGED(57)||CHANGED(58)||CHANGED(53)||CHANGED(54)||CHANGED(55)||
        CHANGED(185)||CHANGED(189)||CHANGED(186)||CHANGED(187)||CHANGED(188))) {
        glEnable(GL_STENCIL_TEST);glStencilMask(s->rs[59]);glStencilFunc(comparison(s->rs[56]),s->rs[57],s->rs[58]);
        glStencilOp(stencil_operation(s->rs[53]),stencil_operation(s->rs[54]),stencil_operation(s->rs[55]));
        if(s->rs[185]) {
            glStencilFuncSeparate(GL_BACK,comparison(s->rs[189]),s->rs[57],s->rs[58]);
            glStencilOpSeparate(GL_BACK,stencil_operation(s->rs[186]),stencil_operation(s->rs[187]),stencil_operation(s->rs[188]));
        }
    } else if(CHANGED(52)&&!s->rs[52])glDisable(GL_STENCIL_TEST);
    if(s->rs[8]==1||s->rs[8]==2)graphics_fail("Point/wireframe fill mode is pending.",62);
    if(CHANGED(195)||CHANGED(175)){
        float bias,slope;memcpy(&bias,&s->rs[195],4);memcpy(&slope,&s->rs[175],4);
        if(bias||slope){glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(slope,ldexpf(bias,24));}else glDisable(GL_POLYGON_OFFSET_FILL);
    }
#undef CHANGED
    memcpy(cached_render_states,s->rs,sizeof cached_render_states);
    memcpy(cached_viewport,s->viewport,sizeof cached_viewport);
    memcpy(cached_scissor,s->scissor,sizeof cached_scissor);
    render_state_invalid=0;
}
// Buffer metadata uses the same guest layout as the macOS D3D9 backend.
enum { BUFFER_LENGTH=0, BUFFER_USAGE=1, BUFFER_DATA=4, BUFFER_NAME=5, BUFFER_DIRTY=6, BUFFER_EPOCH=7 };
enum { GEOMETRY_CACHE_LIMIT=128*1024*1024 };

static int is_geometry_buffer(uint32_t object) {
    if(!object)return 0;
    unsigned kind=G32(object+8);
    return kind==5||kind==6; // K_VB / K_IB
}

static GLuint bind_geometry(uint32_t object,GLenum target,GLuint streaming_buffer) {
    uint32_t length=G32(OF(object,BUFFER_LENGTH));
    GLuint name=0;
    if(G32(object+4)&&!(G32(OF(object,BUFFER_USAGE))&0x200)) { // D3DUSAGE_DYNAMIC
        if(G32(OF(object,BUFFER_EPOCH))==geometry_epoch)
            name=G32(OF(object,BUFFER_NAME));
        if(!name&&length&&length<=GEOMETRY_CACHE_LIMIT-geometry_bytes) {
            glGenBuffers(1,&name);
            if(name) {
                P32(OF(object,BUFFER_NAME),name);
                P32(OF(object,BUFFER_EPOCH),geometry_epoch);
                geometry_bytes+=length;
                atomic_guest_write(OF(object,BUFFER_DIRTY),1);
            }
        }
    }
    bind_geometry_buffer(target,name?name:streaming_buffer);
    if(!name||(atomic_guest_read(OF(object,BUFFER_DIRTY))&&atomic_guest_take(OF(object,BUFFER_DIRTY))))
        glBufferData(target,length,MEM+G32(OF(object,BUFFER_DATA)),name?GL_STATIC_DRAW:GL_STREAM_DRAW);
    return name?name:streaming_buffer;
}

static void attributes(D3DState *s,Program *p,int up,uint32_t stride,int32_t base) {
    if(!s->decl)graphics_fail("FVF vertex declarations are pending.",7);
    uint32_t elements=G32(OF(s->decl,0)),n=G32(OF(s->decl,1));
    uint32_t color_mask=0;
    GLuint stream_bindings[16]={0},bound_stream=0;
    unsigned wanted_attributes=0;
    if(!attribute_state_known) {
        for(int i=0;i<16;++i)glDisableVertexAttribArray(i);
        memset(attribute_cache,0,sizeof attribute_cache);
        enabled_attributes=0;
        attribute_state_known=1;
    }
    for(int attr=0;attr<16;++attr){
        if(p->vi.in_usage[attr]<0)continue;
        int matched=0;
        for(uint32_t i=0;i<n;++i){uint32_t at=elements+i*8;unsigned stream_no=LD(MEM+at,2),offset=LD(MEM+at+2,2),type=MEM[at+4],usage=MEM[at+6],index=MEM[at+7];
            if(stream_no==255)break;
            if(usage!=(unsigned)p->vi.in_usage[attr]||index!=(unsigned)p->vi.in_index[attr])continue;
            if(stream_no>=16)graphics_fail("Invalid vertex stream index.",8);
            uint32_t step=stride;
            if(!up) {
                uint32_t buffer=s->stream[stream_no];
                if(!buffer)graphics_fail("Required vertex stream is missing.",9);
                step=s->stream_stride[stream_no];
                if(!stream_bindings[stream_no])
                    stream_bindings[stream_no]=bind_geometry(buffer,GL_ARRAY_BUFFER,vertex_buffers[stream_no]);
                else if(bound_stream!=stream_bindings[stream_no])
                    bind_geometry_buffer(GL_ARRAY_BUFFER,stream_bindings[stream_no]);
                bound_stream=stream_bindings[stream_no];
            } else if(stream_no)graphics_fail("User-memory draw uses multiple streams.",10);
            GLenum gltype=GL_FLOAT;GLboolean norm=GL_FALSE;int components=type<=3?type+1:4;
            if(type==4||type==5||type==8){gltype=GL_UNSIGNED_BYTE;norm=type!=5;}
            else if(type==6||type==7||type==9||type==10){gltype=GL_SHORT;components=(type==6||type==9)?2:4;norm=type>=9;}
            else if(type==11||type==12){gltype=GL_UNSIGNED_SHORT;components=type==11?2:4;norm=GL_TRUE;}
            else if(type==15||type==16){gltype=GL_HALF_FLOAT;components=type==15?2:4;}
            else if(type>3)graphics_fail("Unsupported vertex declaration element.",11);
            // D3DCOLOR arrives as little-endian BGRA bytes. Other byte formats
            // keep their component order, matching the macOS Metal declarations.
            if(type==4)color_mask|=1u<<attr;
            int64_t start=(int64_t)offset+(up?0:s->stream_off[stream_no])+(int64_t)base*step;
            if(start<0)graphics_fail("Negative vertex offset.",13);
            wanted_attributes|=1u<<attr;
            if(!(enabled_attributes&(1u<<attr)))glEnableVertexAttribArray(attr);
            GLVertexAttribute *cached=&attribute_cache[attr];
            GLuint buffer=up?scratch_vb:bound_stream;
            if(!cached->valid||cached->buffer!=buffer||cached->components!=components||
               cached->type!=gltype||cached->normalized!=norm||cached->stride!=step||cached->offset!=(uintptr_t)start) {
                glVertexAttribPointer(attr,components,gltype,norm,step,(void*)(uintptr_t)start);
                *cached=(GLVertexAttribute){buffer,gltype,components,step,(uintptr_t)start,norm,1};
            }
            if(s->stream_freq[stream_no]>>30)graphics_fail("Instanced vertex stream translation is pending.",14);
            matched=1;break;
        }
        if(!matched)glVertexAttrib4f(attr,0,0,0,1);
    }
    unsigned disable=enabled_attributes&~wanted_attributes;
    for(unsigned i=0;i<16;++i)if(disable&(1u<<i))glDisableVertexAttribArray(i);
    enabled_attributes=wanted_attributes;
    if(p->last_color_mask!=color_mask){
        glUniform1ui(p->color_mask,color_mask);
        p->last_color_mask=color_mask;
    }
}
void d3d_backend_init(D3DState *s){if(!open_context(s->width,s->height))graphics_fail("OpenGL ES device initialization failed.",1);}
void d3d_backend_resize(D3DState *s){host_graphics_reset();d3d_backend_init(s);}
void d3d_backend_created(uint32_t object){
    if(is_geometry_buffer(object)) {
        P32(OF(object,BUFFER_NAME),0);
        P32(OF(object,BUFFER_EPOCH),0);
        atomic_guest_write(OF(object,BUFFER_DIRTY),1);
    } else texture_mark_dirty(object);
}
void d3d_backend_dirty(uint32_t object){
    if(is_geometry_buffer(object))atomic_guest_write(OF(object,BUFFER_DIRTY),1);
    else {texture_mark_dirty(object);++depth_generation;}
}
void d3d_backend_release(uint32_t object){
    // Immutable guest shaders and their bytecode live until the device reset.
    if(!is_geometry_buffer(object)||G32(OF(object,BUFFER_EPOCH))!=geometry_epoch)return;
    GLuint name=G32(OF(object,BUFFER_NAME));
    if(!name)return;
    event_context(NULL);
    glDeleteBuffers(1,&name);
    if(bound_vertex_buffer==name)bound_vertex_buffer=0;
    if(bound_index_buffer==name)bound_index_buffer=0;
    for(unsigned i=0;i<16;++i)if(attribute_cache[i].buffer==name)attribute_cache[i].valid=0;
    geometry_bytes-=G32(OF(object,BUFFER_LENGTH));
    P32(OF(object,BUFFER_NAME),0);
}
void d3d_backend_clear(D3DState *s,uint32_t count,uint32_t rects,uint32_t flags,uint32_t color,float z,uint32_t stencil){
    if(flags&2)++depth_generation;
    render_state_invalid|=INVALID_MASKS|INVALID_SCISSOR;
    target(s);glColorMask(1,1,1,1);glDepthMask(1);glStencilMask(~0u);
    glClearColor(((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,(color>>24)/255.f);glClearDepthf(z);glClearStencil(stencil);
    GLbitfield mask=((flags&1)?GL_COLOR_BUFFER_BIT:0)|((flags&2)?GL_DEPTH_BUFFER_BIT:0)|((flags&4)?GL_STENCIL_BUFFER_BIT:0);
    if(!count){glDisable(GL_SCISSOR_TEST);glClear(mask);}else for(uint32_t i=0;i<count;++i){uint32_t r=rects+i*16;glEnable(GL_SCISSOR_TEST);glScissor(G32(r),G32(r+4),G32(r+8)-G32(r),G32(r+12)-G32(r+4));glClear(mask);}
}
static void query_depth_snapshot(D3DState *s,Program *program){
    // Fragment-depth shaders cannot request early tests. Use a read-only depth
    // snapshot for counting, while the ordinary late hardware test still
    // determines the rendered output. Never sample an attached depth texture.
    int enabled=s->rs[7]&&s->ds;
    glUniform1i(program->depth_func,enabled?s->rs[23]:8);
    if(!enabled){glUniform1f(program->depth_scale,0);return;}
    GLTexture *depth=texture_for(s->ds);
    unsigned w=G32(OF(s->ds,0)),h=G32(OF(s->ds,1));
    if(depth->internal!=GL_DEPTH24_STENCIL8&&depth->internal!=GL_DEPTH_COMPONENT16)graphics_fail("Unsupported depth format for occlusion counting.",77);
    int fresh=!depth_snapshot||depth_snapshot_width!=w||depth_snapshot_height!=h||depth_snapshot_format!=depth->internal;
    if(fresh){
        if(depth_snapshot){
            forget_texture(depth_snapshot);
            glDeleteTextures(1,&depth_snapshot);
        }
        glGenTextures(1,&depth_snapshot);
        select_texture_unit(20);bind_texture(GL_TEXTURE_2D,depth_snapshot);
        glTexStorage2D(GL_TEXTURE_2D,1,depth->internal,w,h);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        depth_snapshot_width=w;depth_snapshot_height=h;depth_snapshot_format=depth->internal;
    }
    if(fresh){
        if(!depth_snapshot_fb)glGenFramebuffers(1,&depth_snapshot_fb);
        bind_framebuffer(GL_DRAW_FRAMEBUFFER,depth_snapshot_fb);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,depth->external==GL_DEPTH_STENCIL?GL_DEPTH_STENCIL_ATTACHMENT:GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depth_snapshot,0);
        GLenum none=GL_NONE;glDrawBuffers(1,&none);
        if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)graphics_fail("Occlusion depth snapshot framebuffer is incomplete.",77);
    }
    if(fresh||depth_snapshot_source!=s->ds||depth_snapshot_generation!=depth_generation){
        bind_framebuffer(GL_DRAW_FRAMEBUFFER,depth_snapshot_fb);
        render_state_invalid|=INVALID_SCISSOR;
        bind_framebuffer(GL_READ_FRAMEBUFFER,output_fb);glDisable(GL_SCISSOR_TEST);
        unsigned copy_w=w<target_width?w:target_width,copy_h=h<target_height?h:target_height;
        glBlitFramebuffer(0,0,copy_w,copy_h,0,0,copy_w,copy_h,GL_DEPTH_BUFFER_BIT,GL_NEAREST);
        bind_framebuffer(GL_FRAMEBUFFER,output_fb);
        if(glGetError()!=GL_NO_ERROR)graphics_fail("Occlusion depth snapshot copy failed.",77);
        depth_snapshot_source=s->ds;depth_snapshot_generation=depth_generation;
    }
    select_texture_unit(20);bind_texture(GL_TEXTURE_2D,depth_snapshot);glBindSampler(20,0);
    glUniform1f(program->depth_scale,depth->internal==GL_DEPTH_COMPONENT16?65535.0f:16777215.0f);
}
static Program *prepare_draw(D3DState *s){
    if(occlusion_active){
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER,0,sample_counter);
    }
target(s);Program *p=program_for(s);use_program(p->name);upload_constants(s,p);bind_samplers(s,p);if(p->manual_depth)query_depth_snapshot(s,p);state(s);return p;}
static void draw_counted(Program *p, GLenum mode, GLsizei count, GLint first, GLenum type, uintptr_t offset) {
    if (p->visibility_only) begin_visibility_draw();
    if (type) glDrawElements(mode,count,type,(const void *)offset);
    else glDrawArrays(mode,first,count);
    if (p->visibility_only) glEndQuery(GL_ANY_SAMPLES_PASSED);
}
void d3d_backend_draw(D3DState *s,uint32_t prim,uint32_t start,uint32_t count,int indexed,int32_t base,uint32_t min,uint32_t vertices){
    (void)min;(void)vertices;Program *p=prepare_draw(s);GLsizei n;GLenum mode=primitive(prim,count,&n);attributes(s,p,0,0,indexed?base:0);
    if(indexed){uint32_t b=s->indices;if(!b)graphics_fail("Index buffer missing.",16);uint32_t format=G32(OF(b,2)),size=format==101?2:4;bind_geometry(b,GL_ELEMENT_ARRAY_BUFFER,scratch_ib);draw_counted(p,mode,n,0,size==2?GL_UNSIGNED_SHORT:GL_UNSIGNED_INT,(uintptr_t)start*size);}
    else draw_counted(p,mode,n,start,0,0);if(glGetError()!=GL_NO_ERROR)graphics_fail("OpenGL ES draw failed.",17);s->draws++;if(s->ds&&s->rs[7]&&s->rs[14])++depth_generation;
}
void d3d_backend_draw_up(D3DState *s,uint32_t prim,uint32_t count,uint32_t data,uint32_t stride,uint32_t indices,uint32_t format,uint32_t min,uint32_t vertices){
    Program *p=prepare_draw(s);GLsizei n;GLenum mode=primitive(prim,count,&n);size_t bytes=(indices?(size_t)min+vertices:(size_t)n)*stride;
    if(bytes>128*1024*1024)graphics_fail("User-memory vertex range too large.",18);
    bind_geometry_buffer(GL_ARRAY_BUFFER,scratch_vb);glBufferData(GL_ARRAY_BUFFER,bytes,MEM+data,GL_STREAM_DRAW);attributes(s,p,1,stride,0);
    if(indices){int size=format==101?2:4;bind_geometry_buffer(GL_ELEMENT_ARRAY_BUFFER,scratch_ib);glBufferData(GL_ELEMENT_ARRAY_BUFFER,n*size,MEM+indices,GL_STREAM_DRAW);draw_counted(p,mode,n,0,size==2?GL_UNSIGNED_SHORT:GL_UNSIGNED_INT,0);}else draw_counted(p,mode,n,0,0,0);
    if(glGetError()!=GL_NO_ERROR)graphics_fail("OpenGL ES user-memory draw failed.",19);s->draws++;if(s->ds&&s->rs[7]&&s->rs[14])++depth_generation;
}

static void blit_surface(uint32_t source,uint32_t sr,uint32_t dest,uint32_t dr,unsigned filter) {
    if(!read_fb)glGenFramebuffers(1,&read_fb);if(!write_fb)glGenFramebuffers(1,&write_fb);
    bind_framebuffer(GL_READ_FRAMEBUFFER,read_fb);attach_surface(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,source);
    bind_framebuffer(GL_DRAW_FRAMEBUFFER,write_fb);attach_surface(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,dest);
    GLenum draw=GL_COLOR_ATTACHMENT0;glDrawBuffers(1,&draw);glReadBuffer(GL_COLOR_ATTACHMENT0);
    if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE||glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)graphics_fail("Surface blit framebuffer incomplete.",52);
    GLint sx0=sr?G32(sr):0,sy0=sr?G32(sr+4):0,sx1=sr?G32(sr+8):G32(OF(source,0)),sy1=sr?G32(sr+12):G32(OF(source,1));
    GLint dx0=dr?G32(dr):0,dy0=dr?G32(dr+4):0,dx1=dr?G32(dr+8):G32(OF(dest,0)),dy1=dr?G32(dr+12):G32(OF(dest,1));
    render_state_invalid|=INVALID_SCISSOR;
    glDisable(GL_SCISSOR_TEST);glBlitFramebuffer(sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,GL_COLOR_BUFFER_BIT,filter==1?GL_NEAREST:GL_LINEAR);
    if(glGetError()!=GL_NO_ERROR)graphics_fail("Surface blit failed.",53);
}
void d3d_backend_stretch(D3DState *s,uint32_t src,uint32_t sr,uint32_t dst,uint32_t dr,uint32_t filter){target(s);blit_surface(src,sr,dst,dr,filter);}
void d3d_backend_colorfill(D3DState *s,uint32_t surf,uint32_t rect,uint32_t color){D3DState copy=*s;copy.rt[0]=surf;copy.rt[1]=copy.rt[2]=copy.rt[3]=copy.ds=0;d3d_backend_clear(&copy,rect?1:0,rect,1,color,1,0);}
void d3d_backend_readback(D3DState *s,uint32_t rt,uint32_t dst){
    target(s);if(!read_fb)glGenFramebuffers(1,&read_fb);bind_framebuffer(GL_READ_FRAMEBUFFER,read_fb);attach_surface(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,rt);glReadBuffer(GL_COLOR_ATTACHMENT0);
    unsigned w=G32(OF(rt,0)),h=G32(OF(rt,1)),fmt=G32(OF(dst,2));
    if(w!=G32(OF(dst,0))||h!=G32(OF(dst,1))||(fmt!=21&&fmt!=22))graphics_fail("Unsupported readback format or size.",54);
    uint8_t *rgba=malloc((size_t)w*h*4);if(!rgba)graphics_fail("Readback allocation failed.",55);
    glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    if(glGetError()!=GL_NO_ERROR){free(rgba);graphics_fail("Render-target readback failed.",56);}
    uint32_t pitch;uint8_t *data=d3d_surf_lock_native(dst,&pitch);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){uint8_t *o=data+y*pitch+x*4,*i=rgba+(y*w+x)*4;o[0]=i[2];o[1]=i[1];o[2]=i[0];o[3]=fmt==22?255:i[3];}
    free(rgba);d3d_surf_unlock_native(dst);
}
// Capture the guest backbuffer after a bounded startup run for visual checks.
static int graphics_capture_impl(D3DState *s,const char *path) {
    if(context==EGL_NO_CONTEXT||!s->frames||!s->backbuffer||!s->width||!s->height||
       s->width>4096||s->height>4096)return 0;
    if(!eglMakeCurrent(display,surface,surface,context))return 0;
    while(glGetError()!=GL_NO_ERROR){}
    if(!read_fb)glGenFramebuffers(1,&read_fb);
    bind_framebuffer(GL_READ_FRAMEBUFFER,read_fb);
    attach_surface(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,s->backbuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return 0;
    if(!eglMakeCurrent(display,surface,surface,context))return 0;
    size_t count=(size_t)s->width*s->height;
    unsigned char *rgba=malloc(count*4);if(!rgba)return 0;
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,s->width,s->height,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    if(glGetError()!=GL_NO_ERROR){free(rgba);return 0;}
    for(size_t i=0;i<count;++i){rgba[i*3]=rgba[i*4];rgba[i*3+1]=rgba[i*4+1];rgba[i*3+2]=rgba[i*4+2];}
    FILE *file=fopen(path,"wb");int okay=0;
    if(file){fprintf(file,"P6\n%u %u\n255\n",s->width,s->height);okay=fwrite(rgba,3,count,file)==count;fclose(file);}
    free(rgba);return okay;
}
int host_graphics_capture(D3DState *s,const char *path) {
    host_graphics_enter();int result=graphics_capture_impl(s,path);host_graphics_leave();return result;
}
void d3d_backend_present(D3DState *s){
    target(s);if(!refresh_surface(s->width,s->height))graphics_fail("Android screen surface is unavailable.",63);
    if(!read_fb)glGenFramebuffers(1,&read_fb);bind_framebuffer(GL_READ_FRAMEBUFFER,read_fb);attach_surface(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,s->backbuffer);glReadBuffer(GL_COLOR_ATTACHMENT0);
    render_state_invalid|=INVALID_SCISSOR;
    bind_framebuffer(GL_DRAW_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);
    double scale=fmin((double)width/s->width,(double)height/s->height);
    int w=(int)(s->width*scale),h=(int)(s->height*scale),x=((int)width-w)/2,y=((int)height-h)/2;
    // A full-window blit overwrites the previous contents. Clear for letterbox bars only.
    if(w!=(int)width||h!=(int)height){
        render_state_invalid|=INVALID_MASKS;
        glColorMask(1,1,1,1);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    }
    glBlitFramebuffer(0,s->height,s->width,0,x,y,x+w,y+h,GL_COLOR_BUFFER_BIT,GL_LINEAR);
    if(active_window&&s->frames>=120)report_frame_work();
    if(glGetError()!=GL_NO_ERROR||!eglSwapBuffers(display,surface))graphics_fail("OpenGL ES presentation failed.",27);s->frames++;
    if(frame_hint)previous_swap_ns=rt_rdtsc();
    if(active_window)atomic_fetch_add_explicit(&window_frames,1,memory_order_relaxed);
    input_frame_draws(s->draws>=previous_frame_draws?s->draws-previous_frame_draws:s->draws);
    previous_frame_draws=s->draws;input_script_tick(s->frames);
    if(input_take_dump_request())atomic_store(&debug_capture_requested,1);

    if(atomic_exchange(&debug_capture_requested,0)){
        const char *root=getenv("FNV_ROOT");if(root){char path[2048];snprintf(path,sizeof path,"%s/.startup-frame.ppm",root);host_graphics_capture(s,path);}}

}

static uint32_t fixture_object(unsigned kind,uint32_t data,uint32_t count) {
    uint32_t object=gmalloc(28);P32(object+8,kind);P32(OF(object,0),data);P32(OF(object,1),count);return object;
}
static int color_vertex_selftest(D3DState *s) {
    const uint32_t vs[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,
        0x0200001f,0x8000000a,0x900f0001,0x0200001f,0x80000000,0xe00f0000,
        0x0200001f,0x8000000a,0xe00f0001,0x02000001,0xe00f0000,0x90e40000,
        0x02000001,0xe00f0001,0x90e40001,0xffff};
    const uint32_t ps[]={0xffff0300,0x0200001f,0x8000000a,0x900f0000,
        0x02000001,0x800f0800,0x90e40000,0xffff};
    const uint8_t declaration[]={0,0,0,0,3,0,0,0,0,0,16,0,4,0,10,0,255,0,0,0,17,0,0,0};
    const float positions[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};
    uint32_t saved_vs=s->vs,saved_ps=s->ps,saved_decl=s->decl;
    uint32_t v=gmalloc(sizeof vs),p=gmalloc(sizeof ps),d=gmalloc(sizeof declaration),vertices=gmalloc(72);
    memcpy(MEM+v,vs,sizeof vs);memcpy(MEM+p,ps,sizeof ps);memcpy(MEM+d,declaration,sizeof declaration);
    for(unsigned i=0;i<3;++i){memcpy(MEM+vertices+24*i,positions+4*i,16);P32(vertices+24*i+16,0x7f204080u);P32(vertices+24*i+20,0x1234abcdu);}
    s->vs=fixture_object(8,v,sizeof vs/4);s->ps=fixture_object(9,p,sizeof ps/4);s->decl=fixture_object(7,d,3);
    int result=0;
    // Reuse the same shader program while changing the declaration: the mask
    // must reset for UBYTE4N, then re-enable for D3DCOLOR. Verify alpha as well.
    const uint8_t types[]={4,8,11,12,4};
    const uint8_t expected[][4]={{32,64,128,127},{128,64,32,127},{64,127,0,255},{64,127,171,18},{32,64,128,127}};
    for(unsigned i=0;i<5;++i){
        MEM[d+12]=types[i];
        d3d_backend_draw_up(s,4,1,vertices,24,0,0,0,0);
        uint8_t pixel[4]={0};glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        for(unsigned channel=0;channel<4;++channel)
            if(abs((int)pixel[channel]-expected[i][channel])>1)result=6+i;
        if(result)break;
    }
    s->vs=saved_vs;s->ps=saved_ps;s->decl=saved_decl;
    return result;
}
static int depth_output_query_selftest(D3DState *s){
    extern uint32_t d3d9_create_texture(uint32_t,int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
    D3DState saved=*s;unsigned before=d3d_backend_event_count();const uint32_t query=0x2ffe0100;
    uint32_t shader[]={0xffff0300,0x05000051,0xa00f0001,0,0,0x3f800000,0x3f800000,
        0x02000001,0x90010800,0xa0000000,0x02000001,0x800f0800,0xa0e40001,0xffff};
    uint32_t code=gmalloc(sizeof shader);memcpy(MEM+code,shader,sizeof shader);
    s->ps=fixture_object(9,code,sizeof shader/4);
    const float positions[]={-1.0625f,1.0625f,0,1,.9375f,1.0625f,0,1,.9375f,-.9375f,0,1,
        -1.0625f,1.0625f,0,1,.9375f,-.9375f,0,1,-1.0625f,-.9375f,0,1};
    uint32_t vertices=gmalloc(sizeof positions);memcpy(MEM+vertices,positions,sizeof positions);
    struct {float clear,depth;unsigned function,clip,scissor,expected;} cases[]={
        {.5f,.25f,2,0,0,256},{.5f,.75f,2,0,0,0},{.5f,.5f,2,0,0,0},
        {.5f,.5f,3,0,0,256},{.5f,.5f,4,0,0,256},{.5f,.75f,5,0,0,256},
        {.5f,.5f,6,0,0,0},{.5f,.5f,7,0,0,256},{.5f,.75f,8,0,0,256},
        {.5f,.25f,1,0,0,0},{.5f,.25f,2,1,0,128},{.5f,.25f,2,0,1,64},
        {.1f,.25f,2,0,0,0},{.9f,.25f,2,0,0,256}};
    int failure=0;
    s->rs[7]=1;s->rs[14]=s->rs[52]=0;s->rs[168]=15;
    for(unsigned format=0;format<2&&!failure;++format){
        uint32_t texture=d3d9_create_texture(0,0,16,16,1,2,format?80:75,0);s->ds=G32(G32(OF(texture,6)));
        for(unsigned i=0;i<sizeof cases/sizeof *cases;++i){
            // Exercise orphan-on-wrap as well as repeated unchanged constants.
            s->psc[0][0]=cases[i].depth;s->rs[23]=cases[i].function;s->rs[152]=cases[i].clip;
            memset(s->clip_planes,0,sizeof s->clip_planes);s->clip_planes[0][0]=1;
            s->rs[174]=cases[i].scissor;s->scissor[0]=8;s->scissor[1]=0;s->scissor[2]=16;s->scissor[3]=8;
            d3d_backend_clear(s,0,0,3,0xffff0000,cases[i].clear,0);
            d3d_backend_draw_up(s,4,2,vertices,16,0,0,0,0);
            uint8_t pixels[16*16*4];glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);unsigned visible=0;
            for(unsigned pixel=0;pixel<256;++pixel)if(!memcmp(pixels+4*pixel,(uint8_t[]){0,0,255,255},4))++visible;
            if(visible!=cases[i].expected){failure=1;break;}
            if(d3d_backend_occlusion_issue(s,query,2)){failure=2;break;}
            d3d_backend_draw_up(s,4,2,vertices,16,0,0,0,0);
            if(d3d_backend_occlusion_issue(s,query,1)){failure=3;break;}
            uint32_t status,count=0;uint64_t deadline=host_uptime_ns()+2000000000ull;
            do{status=d3d_backend_occlusion_poll(query,1,&count);if(status==1){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}}while(status==1&&host_uptime_ns()<deadline);
            if(status||count!=visible){__android_log_print(ANDROID_LOG_ERROR,"FNVGraphics","Depth query fixture format=%u case=%u hardware=%u count=%u status=%#x",format?80:75,i,visible,count,status);failure=4;break;}
        }
    }
    d3d_backend_event_release(query);if(d3d_backend_event_count()!=before)failure=5;
    *s=saved;return failure;
}
static void texture_bridge_fixture(CPU *cpu) {
    extern uint32_t d3d9_create_texture(uint32_t,int,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
    extern uint32_t d3d9_texture_level_data(uint32_t,uint32_t,uint32_t,uint32_t*,uint32_t*);
    extern void d3d9_texture_dirty(uint32_t);
    uint32_t target_tex=d3d9_create_texture(0,0,16,16,1,1,21,0);
    uint32_t back=G32(G32(OF(target_tex,6)));
    D3DState s={0};s.width=s.height=16;s.rt[0]=s.backbuffer=back;s.viewport[2]=s.viewport[3]=16;
    float one=1;memcpy(s.viewport+5,&one,4);s.rs[168]=15;
    uint32_t blue=d3d9_create_texture(0,0,2,2,1,0,21,1),pitch,size;
    uint32_t data=d3d9_texture_level_data(blue,0,0,&pitch,&size);
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)P32(data+y*pitch+x*4,0xff0000ff);
    d3d9_texture_dirty(blue);s.tex[0]=blue;s.ss[0][1]=s.ss[0][2]=s.ss[0][3]=1;s.ss[0][5]=s.ss[0][6]=1;
    const uint32_t vs[]={0xfffe0300,0x0200001f,0x80000000,0x900f0000,
        0x0200001f,0x80000000,0xe00f0000,0x0200001f,0x80000005,0xe00f0001,
        0x05000051,0xa00f0000,0x3f000000,0x3f000000,0,0,
        0x02000001,0xe00f0000,0x90e40000,0x02000001,0xe00f0001,0xa0e40000,0xffff};
    const uint32_t ps[]={0xffff0300,0x0200001f,0x80000005,0x900f0000,
        // Unused declared s1 remains unbound; the linked shader removes it.
        0x0200001f,0x90000000,0xa00f0801,
        0x0200001f,0x90000000,0xa00f0800,0x03000042,0x800f0000,0x90e40000,0xa0e40800,
        0x02000001,0x800f0800,0x80e40000,0xffff};
    uint32_t v=gmalloc(sizeof vs),p=gmalloc(sizeof ps);memcpy(MEM+v,vs,sizeof vs);memcpy(MEM+p,ps,sizeof ps);
    s.vs=fixture_object(8,v,sizeof vs/4);s.ps=fixture_object(9,p,sizeof ps/4);
    const uint8_t decl[]={0,0,0,0,3,0,0,0,255,0,0,0,17,0,0,0};
    uint32_t d=gmalloc(sizeof decl);memcpy(MEM+d,decl,sizeof decl);s.decl=fixture_object(7,d,2);
    const float triangle[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};uint32_t verts=gmalloc(sizeof triangle);memcpy(MEM+verts,triangle,sizeof triangle);
    d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
    uint8_t pixel[4]={0};glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    if(memcmp(pixel,(uint8_t[]){0,0,255,255},4)){RET(cpu,1,0);return;}
    // Use a distinct solid DXT1 color at each mip so both reduced modes can be
    // checked by sampling, even when the game asks for a non-mipmapped filter.
    unsigned saved_skip=texture_mip_skip;
    const uint32_t colors565[]={0xf800,0x07e0,0x001f};
    const uint8_t expected_mip[3][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255}};
    for(unsigned skip=1;skip<=2;++skip){
        uint32_t mip_test=d3d9_create_texture(0,0,8,8,3,0,0x31545844,1);
        for(unsigned level=0;level<3;++level){
            uint32_t blocks=d3d9_texture_level_data(mip_test,0,level,&pitch,&size);
            for(unsigned offset=0;offset+8<=size;offset+=8){
                P32(blocks+offset,colors565[level]);P32(blocks+offset+4,0);
            }
        }
        d3d9_texture_dirty(mip_test);
        s.tex[0]=mip_test;
        texture_mip_skip=skip;
        d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
        texture_mip_skip=saved_skip;
        glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if(memcmp(pixel,expected_mip[skip],4)){RET(cpu,80+skip,0);return;}
        texture_for_use(mip_test,0);
        d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
        glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if(memcmp(pixel,expected_mip[0],4)){RET(cpu,83,0);return;}
    }
    s.tex[0]=blue;
    uint32_t textured_ps=s.ps;
    for(unsigned dimension=2;dimension<=4;++dimension){
        uint32_t code[sizeof ps/4];memcpy(code,ps,sizeof ps);
        code[8]=0x80000000u|(dimension<<27);
        uint32_t bytes=gmalloc(sizeof code);memcpy(MEM+bytes,code,sizeof code);
        s.ps=fixture_object(9,bytes,sizeof code/4);s.tex[0]=0;
        d3d_backend_clear(&s,0,0,1,0xffff0000,1,0);
        d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
        glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if(memcmp(pixel,(uint8_t[]){0,0,0,255},4)){RET(cpu,80+dimension,0);return;}
    }
    s.ps=textured_ps;s.tex[0]=blue;
    d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
    glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    if(memcmp(pixel,(uint8_t[]){0,0,255,255},4)){RET(cpu,85,0);return;}
    // Reused geometry must reflect writes to either buffer after Unlock.
    const uint16_t triangle_indices[]={0,1,2};
    uint32_t index_data=gmalloc(sizeof triangle_indices);
    memcpy(MEM+index_data,triangle_indices,sizeof triangle_indices);
    uint32_t vb=gmalloc(48),ib=gmalloc(48);
    P32(vb+4,1);P32(vb+8,5);P32(OF(vb,BUFFER_LENGTH),sizeof triangle);
    P32(OF(vb,BUFFER_USAGE),0);P32(OF(vb,BUFFER_DATA),verts);
    P32(ib+4,1);P32(ib+8,6);P32(OF(ib,BUFFER_LENGTH),sizeof triangle_indices);
    P32(OF(ib,BUFFER_USAGE),0);P32(OF(ib,2),101);P32(OF(ib,BUFFER_DATA),index_data);
    d3d_backend_created(vb);d3d_backend_created(ib);
    s.stream[0]=vb;s.stream_stride[0]=16;s.indices=ib;
    for(unsigned pass=0;pass<4;++pass) {
        if(pass==2) {
            memset(MEM+index_data,0,sizeof triangle_indices);
            d3d_backend_dirty(ib);
        } else if(pass==3) {
            memcpy(MEM+index_data,triangle_indices,sizeof triangle_indices);
            memset(MEM+verts,0,sizeof triangle);
            d3d_backend_dirty(ib);d3d_backend_dirty(vb);
        }
        d3d_backend_clear(&s,0,0,1,0xffff0000,1,0);
        d3d_backend_draw(&s,4,0,1,1,0,0,3);
        glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        const uint8_t expected[]={pass>=2?255:0,0,pass>=2?0:255,255};
        if(memcmp(pixel,expected,4)){RET(cpu,86+pass,0);return;}
    }
    d3d_backend_release(vb);d3d_backend_release(ib);
    s.stream[0]=s.indices=0;
    memcpy(MEM+verts,triangle,sizeof triangle);
    int color_test=color_vertex_selftest(&s);if(color_test){RET(cpu,color_test,0);return;}
    // D3D9 screen-aligned quad edges lie at pixel centres minus one half.
    // Verify every pixel, including all edges, with a viewport independent of
    // the backbuffer size and then an offset subviewport. This detects both
    // the offset sign and using backbuffer dimensions during offscreen passes.
    uint32_t quad=gmalloc(6*4*sizeof(float));
    s.width=32;s.height=48;
    for(unsigned pass=0;pass<2;++pass){
        unsigned size=pass?8:16,offset=pass?4:0;
        float left=-1.0f-1.0f/size,right=1.0f-1.0f/size;
        float top=1.0f+1.0f/size,bottom=-1.0f+1.0f/size;
        const float positions[]={left,top,0,1,right,top,0,1,right,bottom,0,1,
            left,top,0,1,right,bottom,0,1,left,bottom,0,1};
        memcpy(MEM+quad,positions,sizeof positions);
        s.viewport[0]=s.viewport[1]=offset;s.viewport[2]=s.viewport[3]=size;
        d3d_backend_clear(&s,0,0,1,0xffff0000,1,0);
        d3d_backend_draw_up(&s,4,2,quad,16,0,0,0,0);
        uint8_t pixels[16*16*4];glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
            int inside=x>=offset&&x<offset+size&&y>=offset&&y<offset+size;
            const uint8_t expected[][4]={{255,0,0,255},{0,0,255,255}};
            if(memcmp(pixels+4*(y*16+x),expected[inside],4)){
                __android_log_print(ANDROID_LOG_ERROR,"FNVGraphics","Pixel alignment fixture pass=%u pixel=%u,%u expected=%s got=%u,%u,%u,%u",pass,x,y,inside?"blue":"red",pixels[4*(y*16+x)],pixels[4*(y*16+x)+1],pixels[4*(y*16+x)+2],pixels[4*(y*16+x)+3]);
                RET(cpu,13+pass,0);return;
            }
        }
    }
    s.width=s.height=16;s.viewport[0]=s.viewport[1]=0;s.viewport[2]=s.viewport[3]=16;
    // User planes use original D3D homogeneous clip coordinates, before GL's
    // pixel-centre/Y/depth conversion. Reuse the same program across masks.
    const unsigned samples[][2]={{4,4},{12,4},{4,12},{12,12},{8,4}};
    const uint8_t expected[][5]={{1,1,1,1,1},{0,1,0,1,1},{0,1,0,0,1},{1,1,1,1,1},{0,1,0,1,0}};
    for(unsigned pass=0;pass<5;++pass){
        memset(s.clip_planes,0,sizeof s.clip_planes);
        s.rs[152]=pass==0?0:pass==2?129:1;
        if(pass==0)s.clip_planes[0][3]=-1; // Ignored while disabled.
        else if(pass==3){s.clip_planes[0][2]=1;s.clip_planes[0][3]=.5f;}
        else{s.clip_planes[0][0]=1;if(pass==4)s.clip_planes[0][3]=-.25f;}
        s.clip_planes[7][1]=1;
        d3d_backend_clear(&s,0,0,1,0xffff0000,1,0);
        if(pass==4){
            // Nonuniform W keeps the screen quad fixed but requires homogeneous
            // plane evaluation and perspective-correct interpolation.
            float left=-1.0f-1.0f/16,right=1.0f-1.0f/16;
            float top=1.0f+1.0f/16,bottom=-1.0f+1.0f/16;
            const float positions[]={left,top,0,1,2*right,2*top,0,2,2*right,2*bottom,0,2,
                left,top,0,1,2*right,2*bottom,0,2,left,bottom,0,1};
            memcpy(MEM+quad,positions,sizeof positions);
            d3d_backend_draw_up(&s,4,2,quad,16,0,0,0,0);
        }else d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
        for(unsigned i=0;i<5;++i){
            glReadPixels(samples[i][0],samples[i][1],1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            const uint8_t colors[][4]={{255,0,0,255},{0,0,255,255}};
            if(memcmp(pixel,colors[expected[pass][i]],4)){RET(cpu,15+pass,0);return;}
        }
    }
    s.rs[152]=0;memset(s.clip_planes,0,sizeof s.clip_planes);
    extern int host_occlusion_query_selftest(D3DState *state);
    int occlusion_test=host_occlusion_query_selftest(&s);if(occlusion_test){RET(cpu,40+occlusion_test,0);return;}
    int depth_query=depth_output_query_selftest(&s);if(depth_query){RET(cpu,60+depth_query,0);return;}
    // Translate and execute TEXKILL on this GPU. Negative coordinates discard;
    // positive coordinates must still write the shader's output color.
    uint32_t saved_pixel=s.ps;
    for(unsigned pass=0;pass<2;++pass){
        uint32_t kill_shader[]={0xffff0300,0x05000051,0xa00f0000,0xbf800000,0,0,0,
            0x05000051,0xa00f0001,0,0x3f800000,0,0x3f800000,
            0x02000001,0x800f0000,0xa0e40000,0x01000041,0x800f0000,
            0x02000001,0x800f0800,0xa0e40001,0xffff};
        if(pass)kill_shader[3]=0x3f800000;
        uint32_t code=gmalloc(sizeof kill_shader);memcpy(MEM+code,kill_shader,sizeof kill_shader);
        s.ps=fixture_object(9,code,sizeof kill_shader/4);
        d3d_backend_clear(&s,0,0,1,0xff123456,1,0);
        d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
        glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        const uint8_t expected[][4]={{18,52,86,255},{0,255,0,255}};
        if(memcmp(pixel,expected[pass],4)){RET(cpu,11+pass,0);return;}
    }
    s.ps=saved_pixel;
    d3d_backend_clear(&s,0,0,1,0xff00ff00,1,0);
    uint32_t copy_tex=d3d9_create_texture(0,0,16,16,1,1,21,0),copy=G32(G32(OF(copy_tex,6)));
    d3d_backend_stretch(&s,back,0,copy,0,1);
    d3d_backend_readback(&s,copy,back);
    const uint8_t *bytes=d3d_surf_begin(back);
    int okay=bytes&&!memcmp(bytes,(uint8_t[]){0,255,0,255},4);d3d_surf_end();
    if(!okay){RET(cpu,2,0);return;}
    uint32_t depth=d3d9_create_texture(0,0,16,16,1,2,75,0);s.ds=G32(G32(OF(depth,6)));
    s.rs[52]=1;s.rs[53]=s.rs[54]=1;s.rs[55]=3;s.rs[56]=8;s.rs[57]=1;s.rs[58]=s.rs[59]=255;
    d3d_backend_clear(&s,0,0,4,0,1,0);
    s.rs[168]=0;s.rs[174]=1;s.scissor[2]=8;s.scissor[3]=16;
    d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
    d3d_backend_clear(&s,0,0,1,0xffff0000,1,0);
    s.rs[168]=15;s.rs[174]=0;s.rs[55]=1;s.rs[56]=3;
    d3d_backend_draw_up(&s,4,1,verts,16,0,0,0,0);
    glReadPixels(4,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    if(memcmp(pixel,(uint8_t[]){0,0,255,255},4)){RET(cpu,4,0);return;}
    glReadPixels(12,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    int result=memcmp(pixel,(uint8_t[]){255,0,0,255},4)?5:0;
    extern int host_event_query_selftest(void);
    if(!result){int event=host_event_query_selftest();if(event)result=20+event;}
    if(!result&&test_presentation)d3d_backend_present(&s);
    RET(cpu,result,0);
}
static int texture_bridge_selftest(void) {
    CPU cpu={0};uint32_t stack=0x260f00;SETREG(&cpu,R_ESP,stack);P32(stack,HOST_EXIT_THUNK);
    uint32_t thunk=host_register_import("test","GraphicsBridge",texture_bridge_fixture);
    int trap=rt_run_entry(&cpu,thunk,100000);
    return trap?3:(int)REG(&cpu,R_EAX);
}

static void graphics_lock_trap_fixture(CPU *cpu) {
    host_graphics_enter();rt_trap(cpu,14,0x474c);
}
static _Atomic int transfer_done;
static void *graphics_transfer_fixture(void *unused) {
    (void)unused;int okay=0;uint8_t pixel[4]={0};
    host_graphics_enter();
    if(open_context(8,8)) {
        bind_framebuffer(GL_FRAMEBUFFER,0);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        okay=!memcmp(pixel,(uint8_t[]){255,0,0,255},4);
        glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
        if(glGetError()!=GL_NO_ERROR)okay=0;
    }
    host_graphics_leave();
    host_graphics_yield();
    atomic_store(&transfer_done,1);
    return (void*)(intptr_t)okay;
}
int host_graphics_thread_selftest(void) {
    host_graphics_retain(1);
    host_graphics_reset();host_graphics_enter();
    int okay=open_context(8,8);
    if(okay){bind_framebuffer(GL_FRAMEBUFFER,0);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();}
    host_graphics_leave();if(!okay){host_graphics_retain(0);host_graphics_reset();return 1;}
    // A trapped D3D import must release both the context and serialization lock.
    CPU cpu={0};int trap=rt_run_callback(&cpu,graphics_lock_trap_fixture,1000);
    if(trap!=14){host_graphics_retain(0);host_graphics_reset();return 2;}
    // Retain a context, then require another thread to request and acquire it.
    host_graphics_enter();okay=open_context(8,8);host_graphics_leave();
    if(!okay){host_graphics_retain(0);host_graphics_reset();return 5;}
    pthread_t worker;void *result=NULL;
    atomic_store(&transfer_done,0);
    if(pthread_create(&worker,NULL,graphics_transfer_fixture,NULL)){host_graphics_retain(0);host_graphics_reset();return 3;}
    uint64_t deadline=host_uptime_ns()+2000000000ull;
    while(!atomic_load(&transfer_done)&&host_uptime_ns()<deadline) {
        host_graphics_poll();
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
    int handed_off=atomic_load(&transfer_done);
    host_graphics_retain(0);
    pthread_join(worker,&result);
    host_graphics_enter();uint8_t pixel[4]={0};
    okay=open_context(8,8);
    if(okay){bind_framebuffer(GL_FRAMEBUFFER,0);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);}
    okay=okay&&handed_off&&(intptr_t)result==1&&!memcmp(pixel,(uint8_t[]){0,255,0,255},4);
    host_graphics_reset();host_graphics_leave();return okay?0:4;
}
int host_graphics_surface_selftest(void) {
    test_presentation=1;
    int result=host_graphics_selftest();
    test_presentation=0;return result;
}

#endif // __ANDROID__
