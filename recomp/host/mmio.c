// Multimedia read-only file/memory I/O and bounded RIFF chunk navigation.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <limits.h>
#include "shims.h"
typedef struct {
    int fd, memory, busy;
    uint32_t source,length,position,buffer,capacity,flags;
    uint32_t proc,proc_info;
} Mmio;
#define FCC(a,b,c_,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c_)<<16)|((uint32_t)(d)<<24))
enum {H_MMIO=40,MMIOERR_CANNOTREAD=259,MMIOERR_CHUNKNOTFOUND=265};
enum {MMIOM_READ=0,MMIOM_SEEK=2,MMIOM_OPEN=3,MMIOM_CLOSE=4,MMIOINFO_SIZE=72};
static pthread_mutex_t mmio_lock=PTHREAD_MUTEX_INITIALIZER;
static Mmio *acquire_mmio(uint32_t handle){
    pthread_mutex_lock(&mmio_lock);
    Mmio *m=handle_get(handle,H_MMIO);
    if(m&&m->busy)m=NULL;
    if(m)m->busy=1;
    pthread_mutex_unlock(&mmio_lock);
    return m;
}
static void release_mmio(Mmio *m){
    if(!m)return;
    pthread_mutex_lock(&mmio_lock);
    m->busy=0;
    pthread_mutex_unlock(&mmio_lock);
}
static void destroy_mmio(Mmio *m){
    if(m->fd>=0)close(m->fd);
    if(m->buffer)gfree(m->buffer);
    if(m->proc_info)gfree(m->proc_info);
    free(m);
}
static void detach_mmio(uint32_t handle){
    pthread_mutex_lock(&mmio_lock);
    handle_close(handle);
    pthread_mutex_unlock(&mmio_lock);
}
// Runtime reset follows guest worker shutdown and must not reenter guest code.
int host_mmio_release(int type,void *object){
    if(type!=H_MMIO)return 0;
    destroy_mmio(object);
    return 1;
}
static int32_t mm_proc(CPU *c,Mmio *m,uint32_t message,uint32_t a,uint32_t b){
    CPU saved=*c;
    uint32_t stack=(REG(c,R_ESP)-128-20)&~15u;
    P32(stack,HOST_EXIT_THUNK);
    P32(stack+4,m->proc_info);P32(stack+8,message);P32(stack+12,a);P32(stack+16,b);
    SETREG(c,R_ESP,stack);
    rt_call(c,m->proc);
    if(REG(c,R_ESP)!=stack+20)rt_trap(c,TRAP_UNSUPPORTED,m->proc);
    int32_t result=REG(c,R_EAX);
    *c=saved;
    return result;
}
static int32_t mm_read(CPU *c,Mmio *m,void *out,uint32_t count){
    if(m->position>=m->length)return 0;uint32_t left=m->length-m->position;if(count>left)count=left;
    if(m->memory){memcpy(out,MEM+m->source+m->position,count);m->position+=count;return count;}
    if(m->proc){
        uintptr_t offset=(uintptr_t)out-(uintptr_t)MEM;
        int guest=offset<=UINT32_MAX&&count<=UINT32_MAX-offset;
        // RIFF headers are the only reads directed to the native stack.
        if(!guest&&count>8)return -1;
        uint32_t dest=guest?(uint32_t)offset:m->proc_info+MMIOINFO_SIZE;
        int32_t n=mm_proc(c,m,MMIOM_READ,dest,count);
        if(n<0||(uint32_t)n>count)return -1;
        if(!guest)memcpy(out,MEM+dest,n);
        m->position+=(uint32_t)n;
        return n;
    }
    ssize_t n=pread(m->fd,out,count,m->position);if(n<0)return -1;m->position+=(uint32_t)n;return (int32_t)n;
}
static int mm_seek(CPU *c,Mmio *m,uint64_t position){
    if(position>m->length)return 0;
    if(m->proc&&position!=m->position&&
        (mm_proc(c,m,MMIOM_SEEK,(uint32_t)position,SEEK_SET)<0||G32(m->proc_info+44)!=position))return 0;
    m->position=(uint32_t)position;return 1;
}
#define MMLOCK Mmio *m=acquire_mmio(ARG(c,0))
#define MMRET(value,n) do{release_mmio(m);RET(c,(uint32_t)(value),n);return;}while(0)
static void mmioOpenA_(CPU *c){
    uint32_t name=ARG(c,0),info=ARG(c,1),flags=ARG(c,2),error=0;Mmio *m=calloc(1,sizeof *m);uint32_t handle=0;
    if(!m){if(info)P32(info+12,7);RET(c,0,3);return;}m->fd=-1;m->flags=flags;
    if((flags&3)||(flags&0x1200))error=8;
    else if(info&&G32(info+8)){
        m->proc=G32(info+8);m->proc_info=gmalloc(MMIOINFO_SIZE+8);
        memcpy(MEM+m->proc_info,MEM+info,MMIOINFO_SIZE);
        P32(m->proc_info,flags);P32(m->proc_info+44,0);
        m->busy=1;
        handle=handle_new(H_MMIO,m);
        if(!handle)error=7;
        else{
            P32(m->proc_info+68,handle);
            error=(uint32_t)mm_proc(c,m,MMIOM_OPEN,name,0);
            if(!error){
                int32_t end=mm_proc(c,m,MMIOM_SEEK,0,SEEK_END);
                m->length=G32(m->proc_info+44);
                if(end<0||m->length>INT_MAX||mm_proc(c,m,MMIOM_SEEK,0,SEEK_SET)<0||G32(m->proc_info+44)){
                    mm_proc(c,m,MMIOM_CLOSE,0,0);
                    error=MMIOERR_CANNOTREAD;
                }
            }
            if(!error)release_mmio(m);
        }
    }
    else if(!name&&info&&G32(info+4)==FCC('M','E','M',' ')){
        int32_t length=G32(info+20);m->source=G32(info+24);m->memory=1;
        if(length<0||(!m->source&&length)||(uint64_t)m->source+(uint32_t)length>0x100000000ull)error=11;
        else m->length=(uint32_t)length;
    }else if(name&&GSTR(name)[0]){
        char path[2048];host_path(GSTR(name),path,sizeof path);m->fd=open(path,O_RDONLY);
        struct stat st;if(m->fd<0||fstat(m->fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>INT_MAX)error=257;
        else m->length=(uint32_t)st.st_size;
    }else error=11;
    if(!error&&!handle){handle=handle_new(H_MMIO,m);if(!handle)error=7;}
    if(error){if(handle)detach_mmio(handle);destroy_mmio(m);handle=0;}
    if(info){P32(info+12,error);P32(info+68,handle);}
    RET(c,handle,3);
}
static void mmioClose_(CPU *c){
    MMLOCK;if(!m)MMRET(5,2);
    int32_t result=m->proc?mm_proc(c,m,MMIOM_CLOSE,0,0):0;
    detach_mmio(ARG(c,0));destroy_mmio(m);RET(c,(uint32_t)result,2);
}
static void mmioRead_(CPU *c){MMLOCK;int32_t count=ARG(c,2);if(!m||count<0||(uint64_t)ARG(c,1)+(uint32_t)count>0x100000000ull)MMRET(-1,3);int32_t n=mm_read(c,m,MEM+ARG(c,1),(uint32_t)count);MMRET(n,3);}
static void mmioDescend_(CPU *c){MMLOCK;uint32_t ck=ARG(c,1),parent=ARG(c,2),flags=ARG(c,3);
    if(!m||!ck)MMRET(MMIOERR_CANNOTREAD,4);
    uint32_t want_id=G32(ck),want_type=G32(ck+8);
    uint64_t limit=parent?(uint64_t)G32(parent+12)+G32(parent+4):m->length;if(limit>m->length)MMRET(MMIOERR_CHUNKNOTFOUND,4);
    while((uint64_t)m->position+8<=limit){
        uint32_t at=m->position,hdr[2],type=0;if(mm_read(c,m,hdr,8)!=8)break;
        uint32_t id=hdr[0],size=hdr[1];uint64_t end=(uint64_t)at+8+size;
        if(end>limit)break;
        int container=id==FCC('R','I','F','F')||id==FCC('L','I','S','T');
        if(container&&(size<4||mm_read(c,m,&type,4)!=4))break;
        int match=1;if(flags&0x10)match=id==want_id;
        if(flags&0x20)match=id==FCC('R','I','F','F')&&type==want_type;
        if(flags&0x40)match=id==FCC('L','I','S','T')&&type==want_type;
        if(match){P32(ck,id);P32(ck+4,size);P32(ck+8,type);P32(ck+12,at+8);P32(ck+16,0);MMRET(0,4);}
        if(!mm_seek(c,m,end+(size&1)))break;
    }
    MMRET(MMIOERR_CHUNKNOTFOUND,4);
}
static void mmioAscend_(CPU *c){MMLOCK;uint32_t ck=ARG(c,1);if(!m||!ck)MMRET(MMIOERR_CANNOTREAD,3);uint32_t size=G32(ck+4);int okay=mm_seek(c,m,(uint64_t)G32(ck+12)+size+(size&1));MMRET(okay?0:MMIOERR_CANNOTREAD,3);}
static void mmioGetInfo_(CPU *c){MMLOCK;uint32_t info=ARG(c,1);if(!m||!info)MMRET(MMIOERR_CANNOTREAD,3);
    if(m->proc)memcpy(MEM+info,MEM+m->proc_info,MMIOINFO_SIZE);
    else memset(MEM+info,0,MMIOINFO_SIZE);
    P32(info,m->flags);P32(info+68,ARG(c,0));
    if(m->memory){P32(info+4,FCC('M','E','M',' '));P32(info+20,m->length);P32(info+24,m->source);P32(info+28,m->source+m->position);P32(info+32,m->source+m->length);P32(info+36,m->source+m->length);P32(info+44,m->length);}
    else{if(!m->buffer){m->capacity=65536;m->buffer=gmalloc(m->capacity);}
        P32(info+20,m->capacity);P32(info+24,m->buffer);P32(info+28,m->buffer);P32(info+32,m->buffer);P32(info+36,m->buffer+m->capacity);P32(info+40,m->position);P32(info+44,m->position);}
    MMRET(0,3);
}
static void mmioAdvance_(CPU *c){MMLOCK;uint32_t info=ARG(c,1);if(!m||!info||(ARG(c,2)&3))MMRET(MMIOERR_CANNOTREAD,3);
    uint32_t base=G32(info+24),next=G32(info+28),end=G32(info+32);if(next<base||next>end)MMRET(MMIOERR_CANNOTREAD,3);
    uint64_t position=(uint64_t)G32(info+40)+next-base;if(!mm_seek(c,m,position))MMRET(MMIOERR_CANNOTREAD,3);
    if(m->memory){if(base!=m->source)MMRET(MMIOERR_CANNOTREAD,3);P32(info+28,m->source+m->position);MMRET(0,3);}
    if(!m->buffer||base!=m->buffer)MMRET(MMIOERR_CANNOTREAD,3);
    int32_t n=mm_read(c,m,MEM+m->buffer,m->capacity);if(n<0)MMRET(MMIOERR_CANNOTREAD,3);
    P32(info+24,m->buffer);P32(info+28,m->buffer);P32(info+32,m->buffer+n);P32(info+40,(uint32_t)position);P32(info+44,m->position);MMRET(0,3);
}
const ShimTable mmio_shims={(const Shim[]){
    {"mmioOpenA",mmioOpenA_},{"mmioClose",mmioClose_},{"mmioRead",mmioRead_},{"mmioDescend",mmioDescend_},
    {"mmioAscend",mmioAscend_},{"mmioGetInfo",mmioGetInfo_},{"mmioAdvance",mmioAdvance_}
},7};
#ifdef __ANDROID__
static void fixture_io_proc(CPU *c){
    uint32_t info=ARG(c,0),message=ARG(c,1),a=ARG(c,2),b=ARG(c,3);
    uint32_t source=G32(info+48),length=G32(info+52),position=G32(info+44);
    int32_t result=0;
    switch(message){
        case MMIOM_OPEN: P32(info+48,a);break;
        case MMIOM_CLOSE: P32(source+length,1);break;
        case MMIOM_SEEK: P32(info+44,b==SEEK_END?length:a);break;
        case MMIOM_READ:
            if(b>length-position)b=length-position;
            memcpy(MEM+a,MEM+source+position,b);
            P32(info+44,position+b);result=(int32_t)b;
            break;
        default: result=-1;
    }
    RET(c,(uint32_t)result,4);
}
int host_mmio_selftest(void){
    uint32_t mem=gmalloc(256),info=gmalloc(72),riff=gmalloc(20),chunk=gmalloc(20),output=gmalloc(72);
    // An odd-sized JUNK chunk exercises word padding before the data chunk.
    const uint8_t wav[]={ 'R','I','F','F',28,0,0,0,'W','A','V','E','J','U','N','K',1,0,0,0,99,0,'d','a','t','a',6,0,0,0,1,2,3,4,5,6};
    memcpy(MEM+mem,wav,sizeof wav);P32(info+4,FCC('M','E','M',' '));P32(info+20,sizeof wav);P32(info+24,mem);
    uint32_t open[]={0,info,0};uint32_t h=host_shim_test_call(mmioOpenA_,open,3);if(!h)return 1;
    P32(riff+8,FCC('W','A','V','E'));uint32_t descend[]={h,riff,0,0x20};if(host_shim_test_call(mmioDescend_,descend,4)||G32(riff+12)!=8)return 2;
    P32(chunk,FCC('d','a','t','a'));descend[1]=chunk;descend[2]=riff;descend[3]=0x10;
    if(host_shim_test_call(mmioDescend_,descend,4)||G32(chunk+4)!=6||G32(chunk+12)!=30)return 3;
    uint32_t read[]={h,output,6};if(host_shim_test_call(mmioRead_,read,3)!=6||memcmp(MEM+output,wav+30,6))return 4;
    uint32_t close_args[]={h,0};if(host_shim_test_call(mmioClose_,close_args,2))return 5;
    // A malformed declared extent must not escape the supplied memory range.
    P32(mem+4,1000);h=host_shim_test_call(mmioOpenA_,open,3);descend[0]=h;descend[1]=riff;descend[2]=0;descend[3]=0x20;
    if(host_shim_test_call(mmioDescend_,descend,4)!=MMIOERR_CHUNKNOTFOUND)return 6;
    close_args[0]=h;host_shim_test_call(mmioClose_,close_args,2);
    char path[2048];snprintf(path,sizeof path,"%s/.probe-riff.wav",getenv("FNV_ROOT"));
    FILE *file=fopen(path,"wb");if(!file)return 7;
    int written=fwrite(wav,1,sizeof wav,file)==sizeof wav;fclose(file);if(!written){unlink(path);return 8;}
    uint32_t name=gmalloc(32);memcpy(MEM+name,".probe-riff.wav",16);open[0]=name;memset(MEM+info,0,72);
    h=host_shim_test_call(mmioOpenA_,open,3);if(!h){unlink(path);return 9;}
    uint32_t get_info[]={h,info,0};if(host_shim_test_call(mmioGetInfo_,get_info,3)||host_shim_test_call(mmioAdvance_,get_info,3)){unlink(path);return 10;}
    uint32_t buffer=G32(info+24);if(G32(info+32)-buffer!=sizeof wav||memcmp(MEM+buffer,wav,sizeof wav)){unlink(path);return 11;}
    close_args[0]=h;if(host_shim_test_call(mmioClose_,close_args,2)){unlink(path);return 12;}unlink(path);gfree(name);
    // Exercise the stdcall callback and buffered read used by archived WAVs.
    memcpy(MEM+mem,wav,sizeof wav);P32(mem+sizeof wav,0);
    memset(MEM+info,0,MMIOINFO_SIZE);
    P32(info+8,host_register_import("probe","mmioFixture",fixture_io_proc));
    P32(info+52,sizeof wav);open[0]=mem;open[2]=0x10000;
    h=host_shim_test_call(mmioOpenA_,open,3);if(!h)return 12;
    descend[0]=h;descend[1]=riff;descend[2]=0;descend[3]=0;
    if(host_shim_test_call(mmioDescend_,descend,4))return 13;
    P32(chunk,FCC('d','a','t','a'));descend[1]=chunk;descend[2]=riff;descend[3]=0x10;
    if(host_shim_test_call(mmioDescend_,descend,4)||G32(chunk+4)!=6)return 14;
    get_info[0]=h;
    if(host_shim_test_call(mmioGetInfo_,get_info,3)||host_shim_test_call(mmioAdvance_,get_info,3))return 15;
    buffer=G32(info+24);
    if(G32(info+32)-buffer!=6||memcmp(MEM+buffer,wav+30,6))return 16;
    close_args[0]=h;
    if(host_shim_test_call(mmioClose_,close_args,2)||G32(mem+sizeof wav)!=1)return 17;
    gfree(mem);gfree(info);gfree(riff);gfree(chunk);gfree(output);return 0;
}
#endif
