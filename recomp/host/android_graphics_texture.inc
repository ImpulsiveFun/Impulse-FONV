// Included by android_graphics.c after OF and graphics_fail.
typedef struct { uint32_t object; GLuint name; GLenum target, internal, external, type; unsigned width,height,levels,faces; uint32_t format; } GLTexture;
static GLTexture *texture_objects[65536];
static unsigned texture_count;
static GLTexture null_textures[3];
static GLuint samplers[20], output_fb, read_fb, write_fb;
static uint64_t image_generation, output_generation;
static uint32_t output_targets[4], output_depth;
static int output_valid;
static unsigned active_texture_unit;
static GLuint bound_textures[21][3];
static void select_texture_unit(unsigned unit) {
    if(active_texture_unit==unit)return;
    glActiveTexture(GL_TEXTURE0+unit);
    active_texture_unit=unit;
}
static void bind_texture(GLenum target,GLuint name) {
    unsigned kind=target==GL_TEXTURE_2D?0:target==GL_TEXTURE_CUBE_MAP?1:2;
    GLuint *current=&bound_textures[active_texture_unit][kind];
    if(*current==name)return;
    glBindTexture(target,name);
    *current=name;
}
static void forget_texture(GLuint name) {
    for(unsigned unit=0;unit<21;++unit)
        for(unsigned kind=0;kind<3;++kind)
            if(bound_textures[unit][kind]==name)bound_textures[unit][kind]=0;
}
typedef struct { uint32_t states[7]; int valid; } GLSamplerState;
static GLSamplerState sampler_cache[20];
static unsigned target_width,target_height;
static void reset_textures(void) {
    for(unsigned i=0;i<3;++i)if(null_textures[i].name&&context!=EGL_NO_CONTEXT)glDeleteTextures(1,&null_textures[i].name);
    memset(null_textures,0,sizeof null_textures);
    for(unsigned i=0;i<texture_count;++i){if(context!=EGL_NO_CONTEXT)glDeleteTextures(1,&texture_objects[i]->name);free(texture_objects[i]);}
    memset(texture_objects,0,sizeof texture_objects);texture_count=0;
    output_fb=read_fb=write_fb=0;memset(samplers,0,sizeof samplers);
    output_valid=0;
    active_texture_unit=0;
    memset(bound_textures,0,sizeof bound_textures);
    memset(sampler_cache,0,sizeof sampler_cache);
}
static void surface_format(uint32_t format,GLenum *internal,GLenum *external,GLenum *type) {
    *internal=GL_RGBA8;*external=GL_RGBA;*type=GL_UNSIGNED_BYTE;
    switch(format) {
    case 111:*internal=GL_R16F;*external=GL_RED;*type=GL_HALF_FLOAT;break;
    case 112:*internal=GL_RG16F;*external=GL_RG;*type=GL_HALF_FLOAT;break;
    case 113:*internal=GL_RGBA16F;*type=GL_HALF_FLOAT;break;
    case 114:*internal=GL_R32F;*external=GL_RED;*type=GL_FLOAT;break;
    case 115:*internal=GL_RG32F;*external=GL_RG;*type=GL_FLOAT;break;
    case 116:*internal=GL_RGBA32F;*type=GL_FLOAT;break;
    case 75:case 77:case 0x5a544e49:*internal=GL_DEPTH24_STENCIL8;*external=GL_DEPTH_STENCIL;*type=GL_UNSIGNED_INT_24_8;break;
    case 80:*internal=GL_DEPTH_COMPONENT16;*external=GL_DEPTH_COMPONENT;*type=GL_UNSIGNED_SHORT;break;
    }
}
static uint32_t atomic_guest_read(uint32_t p){return __atomic_load_n((uint32_t*)(void*)(MEM+p),__ATOMIC_ACQUIRE);}
static void atomic_guest_write(uint32_t p,uint32_t v){__atomic_store_n((uint32_t*)(void*)(MEM+p),v,__ATOMIC_RELEASE);}
static uint32_t atomic_guest_take(uint32_t p){return __atomic_exchange_n((uint32_t*)(void*)(MEM+p),0,__ATOMIC_ACQ_REL);}
static void texture_mark_dirty(uint32_t o) {
    unsigned kind=G32(o+8);
    if(kind==4){atomic_guest_write(OF(o,10),1);uint32_t parent=G32(OF(o,7));if(parent)atomic_guest_write(OF(parent,9),1);}
    else if(kind==1||kind==2)atomic_guest_write(OF(o,9),1);
}
static void upload_surface(GLTexture *texture,uint32_t surf,unsigned face,unsigned level) {
    unsigned w=G32(OF(surf,0)),h=G32(OF(surf,1)),pitch=G32(OF(surf,6));
    const uint8_t *source=d3d_surf_begin(surf);
    uint8_t *converted=NULL;
    if(source&&texture->external==GL_RGBA&&texture->type==GL_UNSIGNED_BYTE) {
        converted=malloc((size_t)w*h*4);
        if(!converted){d3d_surf_end();graphics_fail("Texture conversion allocation failed.",40);}
        if(!android_texture_rgba(texture->format,w,h,pitch,source,converted)) {
            free(converted);d3d_surf_end();char text[100];snprintf(text,sizeof text,"Unsupported texture format %#x.",texture->format);graphics_fail(text,41);
        }
    }
    GLenum target=texture->target==GL_TEXTURE_CUBE_MAP?GL_TEXTURE_CUBE_MAP_POSITIVE_X+face:GL_TEXTURE_2D;
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(target,level,texture->internal,w,h,0,texture->external,texture->type,converted?converted:source);
    d3d_surf_end();free(converted);
    if(glGetError()!=GL_NO_ERROR)graphics_fail("Texture upload failed.",42);
    ++image_generation;
}
static GLTexture *texture_for_use(uint32_t object,int bind) {
    if(!object)return NULL;
    unsigned kind=G32(object+8);
    if(kind==4&&G32(OF(object,7)))return texture_for_use(G32(OF(object,7)),bind);
    if(kind!=1&&kind!=2&&kind!=4)graphics_fail("Volume texture support is pending.",43);
    uint32_t host_field=OF(object,kind==4?11:10),dirty_field=OF(object,kind==4?10:9);
    unsigned index=atomic_guest_read(host_field);GLTexture *t=NULL;int fresh=0;
    if(index&&index<=texture_count)t=texture_objects[index-1];
    if(t&&t->object!=object)t=NULL;
    if(!t) {
        if(texture_count==65536)graphics_fail("Texture object limit reached.",44);
        t=calloc(1,sizeof *t);if(!t)graphics_fail("Unable to allocate texture metadata.",45);
        t->object=object;t->width=G32(OF(object,0));t->height=G32(OF(object,1));
        t->levels=kind==4?1:G32(OF(object,2));t->faces=kind==2?6:1;
        t->format=G32(OF(object,kind==4?2:3));t->target=kind==2?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_2D;
        if(!t->width||!t->height||t->width>8192||t->height>8192||!t->levels||t->levels>14){free(t);graphics_fail("Invalid texture dimensions.",46);}
        surface_format(t->format,&t->internal,&t->external,&t->type);
        glGenTextures(1,&t->name);texture_objects[texture_count++]=t;atomic_guest_write(host_field,texture_count);fresh=1;
    }
    int dirty=atomic_guest_read(dirty_field)&&atomic_guest_take(dirty_field);
    if(bind||fresh||dirty)bind_texture(t->target,t->name);
    if(fresh)glTexParameteri(t->target,GL_TEXTURE_MAX_LEVEL,t->levels-1);
    if(fresh||dirty) {
        if(kind==4){atomic_guest_take(OF(object,10));upload_surface(t,object,0,0);}
        else for(unsigned face=0;face<t->faces;++face)for(unsigned level=0;level<t->levels;++level) {
            uint32_t surf=G32(G32(OF(object,6))+4*(face*t->levels+level));
            if(fresh||atomic_guest_take(OF(surf,10))){if(fresh)atomic_guest_take(OF(surf,10));upload_surface(t,surf,face,level);}
        }
    }
    return t;
}
static GLTexture *texture_for(uint32_t object) { return texture_for_use(object,1); }
static void attach_texture(GLenum framebuffer,GLenum attachment,uint32_t surf,GLTexture *t) {
    if(!surf){glFramebufferTexture2D(framebuffer,attachment,GL_TEXTURE_2D,0,0);return;}
    uint32_t parent=G32(OF(surf,7)),where=parent?G32(OF(surf,9)):0;
    GLenum type=t->target==GL_TEXTURE_CUBE_MAP?GL_TEXTURE_CUBE_MAP_POSITIVE_X+(where>>16):GL_TEXTURE_2D;
    glFramebufferTexture2D(framebuffer,attachment,type,t->name,where&65535);
}
static void attach_surface(GLenum framebuffer,GLenum attachment,uint32_t surf) {
    attach_texture(framebuffer,attachment,surf,texture_for_use(surf,0));
}
static void bind_target(D3DState *s) {
    if(context==EGL_NO_CONTEXT&&!open_context(s->width,s->height))graphics_fail("Unable to create OpenGL ES output.",1);
    if(!retained_on_thread&&eglGetCurrentContext()!=context&&!eglMakeCurrent(display,surface,surface,context))graphics_fail("Unable to transfer the GL context to the rendering thread.",2);
    if(!output_fb)glGenFramebuffers(1,&output_fb);
    bind_framebuffer(GL_FRAMEBUFFER,output_fb);
    GLTexture *colors[4];
    for(int i=0;i<4;++i)colors[i]=texture_for_use(s->rt[i],0);
    GLTexture *depth=texture_for_use(s->ds,0);
    target_width=s->rt[0]?G32(OF(s->rt[0],0)):s->width;
    target_height=s->rt[0]?G32(OF(s->rt[0],1)):s->height;
    // Check uploads first because glTexImage2D can replace an attached image.
    if(output_valid&&output_generation==image_generation&&output_depth==s->ds&&
       !memcmp(output_targets,s->rt,sizeof output_targets))return;
    output_valid=0;
    GLenum buffers[4];int count=0;
    for(int i=0;i<4;++i){attach_texture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0+i,s->rt[i],colors[i]);buffers[i]=s->rt[i]?GL_COLOR_ATTACHMENT0+i:GL_NONE;if(s->rt[i])count=i+1;}
    glDrawBuffers(count,buffers);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,0,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
    if(depth)attach_texture(GL_FRAMEBUFFER,depth->external==GL_DEPTH_STENCIL?GL_DEPTH_STENCIL_ATTACHMENT:GL_DEPTH_ATTACHMENT,s->ds,depth);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)graphics_fail("Incomplete render-target framebuffer.",47);
    memcpy(output_targets,s->rt,sizeof output_targets);
    output_depth=s->ds;output_generation=image_generation;output_valid=1;
}
static GLenum address_mode(unsigned mode){switch(mode){case 0:case 1:return GL_REPEAT;case 2:return GL_MIRRORED_REPEAT;case 3:return GL_CLAMP_TO_EDGE;default:graphics_fail("Texture border addressing is pending.",48);return GL_CLAMP_TO_EDGE;}}
static GLTexture *null_texture_for(unsigned dimension){
    // D3D9 unbound samplers yield opaque black (Wine d3d9 visual tests).
    // Bind a complete texture of the declared type so prior bindings cannot leak.
    unsigned index=dimension==3?1:dimension==4?2:0;
    GLTexture *t=&null_textures[index];
    t->target=index==1?GL_TEXTURE_CUBE_MAP:index==2?GL_TEXTURE_3D:GL_TEXTURE_2D;
    if(!t->name){
        const uint8_t black[]={0,0,0,255};
        glGenTextures(1,&t->name);bind_texture(t->target,t->name);
        glTexParameteri(t->target,GL_TEXTURE_MAX_LEVEL,0);
        if(index==2)glTexImage3D(t->target,0,GL_RGBA8,1,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,black);
        else for(unsigned face=0;face<(index==1?6u:1u);++face)
            glTexImage2D(index==1?GL_TEXTURE_CUBE_MAP_POSITIVE_X+face:t->target,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,black);
        t->external=GL_RGBA;
        if(glGetError()!=GL_NO_ERROR)graphics_fail("Null sampler texture allocation failed.",49);
    }else bind_texture(t->target,t->name);
    return t;
}
static void bind_samplers(D3DState *s,Program *p) {
    if(!samplers[0])glGenSamplers(20,samplers);
    for(int stage=0;stage<2;++stage) {
        SMInfo *info=stage?&p->vi:&p->pi;
        for(int i=0;i<(stage?4:16);++i) {
            unsigned slot=stage?16+i:i;
            if(!(p->active_samplers&(1u<<slot)))continue;
            select_texture_unit(slot);
            GLTexture *t=texture_for(s->tex[slot]);
            if(!t)t=null_texture_for(info->sampler_dim[i]);
            if(t->external==GL_DEPTH_COMPONENT||t->external==GL_DEPTH_STENCIL)graphics_fail("Depth texture sampling bridge is pending.",50);
            if(s->ss[slot][11])graphics_fail("sRGB texture sampling bridge is pending.",51);
            GLuint sampler=samplers[slot];
            GLSamplerState *cached=&sampler_cache[slot];
            if(!cached->valid)glBindSampler(slot,sampler);
            if(cached->valid&&!memcmp(cached->states,s->ss[slot]+1,sizeof cached->states))continue;
            glSamplerParameteri(sampler,GL_TEXTURE_WRAP_S,address_mode(s->ss[slot][1]));glSamplerParameteri(sampler,GL_TEXTURE_WRAP_T,address_mode(s->ss[slot][2]));glSamplerParameteri(sampler,GL_TEXTURE_WRAP_R,address_mode(s->ss[slot][3]));
            unsigned min=s->ss[slot][6],mag=s->ss[slot][5],mip=s->ss[slot][7];
            GLenum filter=mip?(mip==2?(min==1?GL_NEAREST_MIPMAP_LINEAR:GL_LINEAR_MIPMAP_LINEAR):(min==1?GL_NEAREST_MIPMAP_NEAREST:GL_LINEAR_MIPMAP_NEAREST)):(min==1?GL_NEAREST:GL_LINEAR);
            glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,filter);glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,mag==1?GL_NEAREST:GL_LINEAR);
            memcpy(cached->states,s->ss[slot]+1,sizeof cached->states);
            cached->valid=1;
        }
    }
    // Uploads bind their texture targets, but do not alter the active framebuffer.
}
