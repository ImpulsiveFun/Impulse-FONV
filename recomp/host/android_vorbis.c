#ifdef __ANDROID__
// Win32 vorbisfile ABI over Xiph's native decoder. Decoder state stays native;
// guest callbacks execute synchronously inside the existing execution guard.
#include <vorbis/vorbisfile.h>
#include <vorbis/vorbisenc.h>
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include "shims.h"

typedef struct {
    uint32_t key, source, read_fn, seek_fn, close_fn, tell_fn, scratch, scratch_size, info;
    CPU *cpu;
    OggVorbis_File decoder;
    int busy;
} Decoder;
static Decoder *decoders[512];
static pthread_mutex_t decoder_lock=PTHREAD_MUTEX_INITIALIZER;

static uint32_t guest_callback(CPU *cpu,uint32_t function,const uint32_t *arguments,int count) {
    CPU saved=*cpu;
    uint32_t stack=(REG(cpu,R_ESP)-128-(uint32_t)count*4)&~15u;
    P32(stack,HOST_EXIT_THUNK);
    for(int i=0;i<count;++i)P32(stack+4+4*i,arguments[i]);
    SETREG(cpu,R_ESP,stack);
    rt_call(cpu,function);
    if(REG(cpu,R_ESP)!=stack+4)rt_trap(cpu,TRAP_UNSUPPORTED,function);
    uint32_t result=REG(cpu,R_EAX);*cpu=saved;return result;
}
static size_t read_callback(void *output,size_t size,size_t count,void *source) {
    Decoder *d=source;
    if(!size||!count)return 0;
    if(size>UINT32_MAX||count>UINT32_MAX||count>16*1024*1024/size)return (size_t)-1;
    uint32_t bytes=(uint32_t)(size*count);
    if(bytes>d->scratch_size){if(d->scratch)gfree(d->scratch);d->scratch=gmalloc(bytes);d->scratch_size=bytes;}
    uint32_t args[]={d->scratch,(uint32_t)size,(uint32_t)count,d->source};
    uint32_t got=guest_callback(d->cpu,d->read_fn,args,4);
    if((int32_t)got<0||got>count)return (size_t)-1;
    memcpy(output,MEM+d->scratch,(size_t)got*size);return got;
}
static int seek_callback(void *source,ogg_int64_t offset,int whence) {
    Decoder *d=source;uint64_t bits=(uint64_t)offset;
    uint32_t args[]={d->source,(uint32_t)bits,(uint32_t)(bits>>32),(uint32_t)whence};
    return (int32_t)guest_callback(d->cpu,d->seek_fn,args,4);
}
static long tell_callback(void *source) {
    Decoder *d=source;return (int32_t)guest_callback(d->cpu,d->tell_fn,&d->source,1);
}
static Decoder *acquire_decoder(uint32_t key) {
    pthread_mutex_lock(&decoder_lock);Decoder *found=NULL;
    for(int i=0;i<512;++i)if(decoders[i]&&decoders[i]->key==key){if(!decoders[i]->busy){found=decoders[i];found->busy=1;}break;}
    pthread_mutex_unlock(&decoder_lock);return found;
}
static void release_decoder(Decoder *d) {pthread_mutex_lock(&decoder_lock);d->busy=0;pthread_mutex_unlock(&decoder_lock);}
static void detach_decoder(Decoder *d) {
    pthread_mutex_lock(&decoder_lock);
    for(int i=0;i<512;++i)if(decoders[i]==d){decoders[i]=NULL;break;}
    pthread_mutex_unlock(&decoder_lock);
}
static void destroy_decoder(Decoder *d) {
    // close_func is deliberately NULL: guest close is invoked after native
    // teardown, so a trapped guest callback cannot cause a second native free.
    ov_clear(&d->decoder);
    if(d->scratch)gfree(d->scratch);if(d->info)gfree(d->info);free(d);
}
void host_vorbis_reset(void) {
    // All guest workers have joined. Never invoke guest callbacks during reset.
    for(int i=0;i<512;++i)if(decoders[i]){Decoder *d=decoders[i];decoders[i]=NULL;destroy_decoder(d);}
}
static void ov_open_callbacks_(CPU *cpu) {
    uint32_t key=ARG(cpu,1),initial=ARG(cpu,2);int32_t initial_size=ARG(cpu,3);
    if(!key||!ARG(cpu,4)||initial_size<0||initial_size>16*1024*1024||
       (initial_size&&!initial)){RET(cpu,(uint32_t)OV_EINVAL,0);return;}
    Decoder *d=calloc(1,sizeof *d);
    if(!d){RET(cpu,(uint32_t)OV_EFAULT,0);return;}
    d->key=key;d->source=ARG(cpu,0);d->read_fn=ARG(cpu,4);d->seek_fn=ARG(cpu,5);
    d->close_fn=ARG(cpu,6);d->tell_fn=ARG(cpu,7);d->cpu=cpu;d->busy=1;
    int slot=-1,duplicate=0;pthread_mutex_lock(&decoder_lock);
    for(int i=0;i<512;++i){if(decoders[i]&&decoders[i]->key==key)duplicate=1;if(!decoders[i]&&slot<0)slot=i;}
    if(!duplicate&&slot>=0)decoders[slot]=d;
    pthread_mutex_unlock(&decoder_lock);
    if(duplicate||slot<0){free(d);RET(cpu,(uint32_t)OV_EFAULT,0);return;}
    ov_callbacks callbacks={read_callback,d->seek_fn&&d->tell_fn?seek_callback:NULL,NULL,
                             d->seek_fn&&d->tell_fn?tell_callback:NULL};
    int result=ov_open_callbacks(d,&d->decoder,initial?(char*)MEM+initial:NULL,initial_size,callbacks);
    if(result){detach_decoder(d);destroy_decoder(d);P32(key,0);P32(key+4,0);}
    else {P32(key,d->source);P32(key+4,(uint32_t)ov_seekable(&d->decoder));release_decoder(d);}
    RET(cpu,(uint32_t)result,0);
}
static void ov_clear_(CPU *cpu) {
    uint32_t key=ARG(cpu,0);Decoder *d=acquire_decoder(key);
    if(!d){RET(cpu,key&&!G32(key)?0:(uint32_t)OV_EINVAL,0);return;}
    uint32_t close_fn=d->close_fn,source=d->source;
    detach_decoder(d);destroy_decoder(d);P32(key,0);P32(key+4,0);
    if(close_fn)guest_callback(cpu,close_fn,&source,1);
    RET(cpu,0,0);
}
static void ov_info_(CPU *cpu) {
    Decoder *d=acquire_decoder(ARG(cpu,0));
    if(!d){RET(cpu,0,0);return;}
    d->cpu=cpu;vorbis_info *info=ov_info(&d->decoder,(int32_t)ARG(cpu,1));uint32_t out=0;
    if(info){
        if(!d->info)d->info=gmalloc(32);out=d->info;
        P32(out,info->version);P32(out+4,info->channels);P32(out+8,(uint32_t)info->rate);
        P32(out+12,(uint32_t)info->bitrate_upper);P32(out+16,(uint32_t)info->bitrate_nominal);
        P32(out+20,(uint32_t)info->bitrate_lower);P32(out+24,(uint32_t)info->bitrate_window);P32(out+28,0);
    }
    release_decoder(d);RET(cpu,out,0);
}
static void ov_pcm_total_(CPU *cpu) {
    Decoder *d=acquire_decoder(ARG(cpu,0));ogg_int64_t value=-1;
    if(d){d->cpu=cpu;value=ov_pcm_total(&d->decoder,(int32_t)ARG(cpu,1));release_decoder(d);}
    uint64_t bits=(uint64_t)value;SETREG(cpu,R_EDX,(uint32_t)(bits>>32));RET(cpu,(uint32_t)bits,0);
}
static void ov_seekable_(CPU *cpu) {
    Decoder *d=acquire_decoder(ARG(cpu,0));long value=0;
    if(d){value=ov_seekable(&d->decoder);release_decoder(d);}RET(cpu,(uint32_t)value,0);
}
static void ov_read_(CPU *cpu) {
    uint32_t out=ARG(cpu,1),bits=ARG(cpu,6);int32_t length=ARG(cpu,2);int word=ARG(cpu,4);
    if(!out||length<0||length>16*1024*1024||(uint64_t)out+(uint32_t)length>0x100000000ull||
       (word!=1&&word!=2)){RET(cpu,(uint32_t)OV_EINVAL,0);return;}
    Decoder *d=acquire_decoder(ARG(cpu,0));
    if(!d){RET(cpu,(uint32_t)OV_EINVAL,0);return;}
    d->cpu=cpu;
    long result=ov_read(&d->decoder,(char*)MEM+out,length,ARG(cpu,3)!=0,word,ARG(cpu,5)!=0,bits?(int*)(void*)(MEM+bits):NULL);
    release_decoder(d);RET(cpu,(uint32_t)result,0);
}
const ShimTable android_vorbis_shims={(const Shim[]){
    {"ov_open_callbacks",ov_open_callbacks_},{"ov_clear",ov_clear_},{"ov_info",ov_info_},
    {"ov_pcm_total",ov_pcm_total_},{"ov_seekable",ov_seekable_},{"ov_read",ov_read_}
},6};

// A generated silence stream tests the Win32 callback ABI and struct marshalling
// without embedding or playing any game audio.
static uint8_t *test_ogg;static size_t test_ogg_size;
static void append_page(const ogg_page *page) {
    size_t n=(size_t)page->header_len+page->body_len;
    uint8_t *next=realloc(test_ogg,test_ogg_size+n);
    if(!next)return;
    test_ogg=next;memcpy(next+test_ogg_size,page->header,page->header_len);
    memcpy(next+test_ogg_size+page->header_len,page->body,page->body_len);test_ogg_size+=n;
}
static int encode_test_stream(void) {
    vorbis_info info;vorbis_info_init(&info);
    if(vorbis_encode_init_vbr(&info,1,8000,.1f)){vorbis_info_clear(&info);return 0;}
    vorbis_comment comment;vorbis_comment_init(&comment);
    vorbis_dsp_state dsp;vorbis_block block;ogg_stream_state stream;
    vorbis_analysis_init(&dsp,&info);vorbis_block_init(&dsp,&block);ogg_stream_init(&stream,0x464e56);
    ogg_packet headers[3],packet;ogg_page page;
    vorbis_analysis_headerout(&dsp,&comment,&headers[0],&headers[1],&headers[2]);
    for(int i=0;i<3;++i)ogg_stream_packetin(&stream,&headers[i]);
    while(ogg_stream_flush(&stream,&page))append_page(&page);
    for(int pass=0;pass<2;++pass){
        if(!pass){float **pcm=vorbis_analysis_buffer(&dsp,800);memset(pcm[0],0,800*sizeof(float));vorbis_analysis_wrote(&dsp,800);}
        else vorbis_analysis_wrote(&dsp,0);
        while(vorbis_analysis_blockout(&dsp,&block)==1){
            vorbis_analysis(&block,NULL);vorbis_bitrate_addblock(&block);
            while(vorbis_bitrate_flushpacket(&dsp,&packet)){
                ogg_stream_packetin(&stream,&packet);
                while(ogg_stream_pageout(&stream,&page))append_page(&page);
            }
        }
    }
    while(ogg_stream_flush(&stream,&page))append_page(&page);
    ogg_stream_clear(&stream);vorbis_block_clear(&block);vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);vorbis_info_clear(&info);return test_ogg_size>0;
}
static void test_read(CPU *cpu) {
    uint32_t out=ARG(cpu,0),size=ARG(cpu,1),count=ARG(cpu,2),source=ARG(cpu,3),position=G32(source+8);
    uint32_t remaining=G32(source+4)-position,n=size?remaining/size:0;if(n>count)n=count;
    memcpy(MEM+out,MEM+G32(source)+position,(size_t)n*size);P32(source+8,position+n*size);RET(cpu,n,0);
}
static void test_seek(CPU *cpu) {
    uint32_t source=ARG(cpu,0);int whence=ARG(cpu,3);uint64_t bits=((uint64_t)ARG(cpu,2)<<32)|ARG(cpu,1);
    int64_t offset=(int64_t)bits,base=whence==SEEK_SET?0:whence==SEEK_CUR?G32(source+8):G32(source+4),position=base+offset;
    if(position<0||position>G32(source+4)){RET(cpu,0xffffffffu,0);return;}P32(source+8,(uint32_t)position);RET(cpu,0,0);
}
static void test_tell(CPU *cpu){RET(cpu,G32(ARG(cpu,0)+8),0);}
static void test_close(CPU *cpu){uint32_t source=ARG(cpu,0);P32(source+12,G32(source+12)+1);RET(cpu,0,0);}
static uint64_t test_cdecl(ShimFn fn,const uint32_t *args,int count) {
    CPU cpu={0};const uint32_t stack=0x2ffe0000;SETREG(&cpu,R_ESP,stack);P32(stack,HOST_EXIT_THUNK);
    for(int i=0;i<count;++i)P32(stack+4+i*4,args[i]);fn(&cpu);
    if(REG(&cpu,R_ESP)!=stack+4)return UINT64_MAX;
    return ((uint64_t)REG(&cpu,R_EDX)<<32)|REG(&cpu,R_EAX);
}
static void vorbis_fixture(CPU *cpu) {
    uint32_t data=gmalloc((uint32_t)test_ogg_size),source=gmalloc(16),key=gmalloc(1024),output=gmalloc(4096),link=gmalloc(4);
    memcpy(MEM+data,test_ogg,test_ogg_size);P32(source,data);P32(source+4,(uint32_t)test_ogg_size);
    uint32_t args[]={source,key,0,0,host_register_import("test","VorbisRead",test_read),
        host_register_import("test","VorbisSeek",test_seek),host_register_import("test","VorbisClose",test_close),host_register_import("test","VorbisTell",test_tell)};
    if(test_cdecl(ov_open_callbacks_,args,8)){RET(cpu,1,0);return;}
    uint32_t query[]={key,0xffffffffu};uint64_t total=test_cdecl(ov_pcm_total_,query,2);
    if(total!=800){RET(cpu,2,0);return;}
    uint32_t info=(uint32_t)test_cdecl(ov_info_,query,2);
    if(!info||G32(info+4)!=1||G32(info+8)!=8000||test_cdecl(ov_seekable_,&key,1)!=1){RET(cpu,3,0);return;}
    uint32_t read[]={key,output,4096,0,2,1,link};unsigned bytes=0;
    for(int i=0;i<32;++i){
        int32_t n=(uint32_t)test_cdecl(ov_read_,read,7);
        if(n<0){RET(cpu,4,0);return;}if(!n)break;
        for(int j=0;j<n;++j)if(MEM[output+j]){RET(cpu,5,0);return;}bytes+=n;
    }
    if(bytes!=1600||G32(link)!=0){RET(cpu,6,0);return;}
    if(test_cdecl(ov_clear_,&key,1)||G32(source+12)!=1){RET(cpu,7,0);return;}
    if(test_cdecl(ov_clear_,&key,1)||G32(source+12)!=1){RET(cpu,8,0);return;}
    gfree(data);gfree(source);gfree(key);gfree(output);gfree(link);RET(cpu,0,0);
}
int host_vorbis_selftest(void) {
    test_ogg=NULL;test_ogg_size=0;
    if(!encode_test_stream()){free(test_ogg);test_ogg=NULL;return 10;}
    CPU cpu={0};const uint32_t stack=0x270f00;SETREG(&cpu,R_ESP,stack);P32(stack,HOST_EXIT_THUNK);
    uint32_t thunk=host_register_import("test","VorbisFixture",vorbis_fixture);
    int trap=rt_run_entry(&cpu,thunk,200000),result=trap?20+trap:(int)REG(&cpu,R_EAX);
    host_vorbis_reset();free(test_ogg);test_ogg=NULL;test_ogg_size=0;return result;
}
#endif
