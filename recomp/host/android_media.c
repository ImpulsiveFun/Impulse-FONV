#ifdef __ANDROID__
// DirectShow interface layouts follow the supplied macOS bridge. Playback is
// delegated to Android MediaPlayer; no proprietary media is embedded.
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <android/log.h>
#include "com.h"
static JavaVM *media_vm;
static jclass media_class;
static jmethodID dispatch_method,media_test_method;
int host_media_bind(JNIEnv *env) {
    if(media_class)return dispatch_method&&media_test_method;
    if((*env)->GetJavaVM(env,&media_vm)!=JNI_OK)return 0;
    jclass local=(*env)->FindClass(env,"org/fnvprobe/MediaBridge");
    if(!local){(*env)->ExceptionClear(env);return 0;}
    media_class=(*env)->NewGlobalRef(env,local);(*env)->DeleteLocalRef(env,local);
    dispatch_method=(*env)->GetStaticMethodID(env,media_class,"dispatch","(IILjava/lang/String;D)D");
    media_test_method=(*env)->GetStaticMethodID(env,media_class,"selfTest","(Ljava/lang/String;)I");
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);return 0;}
    return dispatch_method&&media_test_method;
}
static double media_call(uint32_t id,int op,const char *path,double value) {
    if(!media_vm||!dispatch_method)return -1;
    JNIEnv *env=NULL;int attached=0;
    jint state=(*media_vm)->GetEnv(media_vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*media_vm)->AttachCurrentThread(media_vm,&env,NULL)!=JNI_OK)return -1;attached=1;}
    else if(state!=JNI_OK)return -1;
    jstring text=path?(*env)->NewStringUTF(env,path):NULL;
    double result=(*env)->CallStaticDoubleMethod(env,media_class,dispatch_method,(jint)id,(jint)op,text,(jdouble)value);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);result=-1;}
    if(text)(*env)->DeleteLocalRef(env,text);
    if(attached)(*media_vm)->DetachCurrentThread(media_vm);
    return result;
}
int host_media_selftest(JNIEnv *env) {
    jstring root=(*env)->NewStringUTF(env,getenv("FNV_ROOT"));
    int result=(*env)->CallStaticIntMethod(env,media_class,media_test_method,root);
    (*env)->DeleteLocalRef(env,root);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);return 20;}
    return result;
}
enum { I_GRAPH,I_CONTROL,I_EVENT,I_AUDIO,I_SEEKING,I_POSITION,I_SINK,I_COUNT };
typedef struct {uint32_t id,refs,internal_refs,ifaces[I_COUNT],filter;int volume,balance,reported,cancel_complete;uint32_t events[32][3],event_head,event_count;} Graph;
static Graph *graphs[64];static uint32_t next_id=1,vt[I_COUNT];
static pthread_mutex_t graph_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local Graph *joining_graph;
static _Thread_local unsigned filter_release_depth;
static Graph *graph(CPU *c){uint32_t id=G32(COM_DATA(COM_THIS(c)));for(int i=0;i<64;++i)if(graphs[i]&&graphs[i]->id==id)return graphs[i];return NULL;}
static int which(uint32_t iid){switch(G32(iid)){
case 0:case 0x56a8689f:case 0x56a868a9:case 0x36b73882:return I_GRAPH;
case 0x56a868b1:return I_CONTROL;case 0x56a868b6:case 0x56a868c0:return I_EVENT;
case 0x56a868a2:return I_SINK;case 0x56a868b3:return I_AUDIO;case 0x36b73880:return I_SEEKING;case 0x56a868b2:return I_POSITION;default:return -1;}}
static uint32_t iface(Graph *g,int i){if(!g->ifaces[i]){g->ifaces[i]=com_new(vt[i],4);P32(COM_DATA(g->ifaces[i]),g->id);}return g->ifaces[i];}
#define LOCK Graph *g;pthread_mutex_lock(&graph_lock);g=graph(c)
#define DONE(value,n) do{pthread_mutex_unlock(&graph_lock);RET(c,(uint32_t)(value),n);return;}while(0)
static void DS_QI(CPU *c){LOCK;int i=which(ARG(c,1));uint32_t out=ARG(c,2);if(!g||i<0){P32(out,0);DONE(0x80004002u,3);}++g->refs;if(i==I_SINK&&joining_graph==g)++g->internal_refs;P32(out,iface(g,i));DONE(0,3);}
static void DS_AddRef(CPU *c){LOCK;uint32_t n=g?++g->refs:0;DONE(n,1);}
static uint32_t guest_method(CPU *cpu,uint32_t object,unsigned method,const uint32_t *extra,unsigned count) {
    CPU saved=*cpu;uint32_t stack=(REG(cpu,R_ESP)-128-4*(count+1))&~15u;
    P32(stack,HOST_EXIT_THUNK);P32(stack+4,object);
    for(unsigned i=0;i<count;++i)P32(stack+8+i*4,extra[i]);SETREG(cpu,R_ESP,stack);
    rt_call(cpu,G32(G32(object)+4*method));
    if(REG(cpu,R_ESP)!=stack+8+count*4)rt_trap(cpu,TRAP_UNSUPPORTED,G32(object));
    uint32_t value=REG(cpu,R_EAX);*cpu=saved;return value;
}
static void release_filter(CPU *c,uint32_t filter) {
    if(filter){++filter_release_depth;uint32_t args[]={0,0};guest_method(c,filter,13,args,2);guest_method(c,filter,2,NULL,0);--filter_release_depth;}
}
static void DS_Release(CPU *c){LOCK;
    if(g&&filter_release_depth&&COM_THIS(c)==g->ifaces[I_SINK]&&g->internal_refs)--g->internal_refs;
    uint32_t n=g&&g->refs?--g->refs:0;
    // Break the graph -> source -> event-sink -> graph cycle when the final
    // external reference goes away. A temporary reference keeps callbacks safe.
    if(g&&g->filter&&(n==0||n==g->internal_refs)){
        uint32_t filter=g->filter;g->filter=0;++g->refs;
        pthread_mutex_unlock(&graph_lock);release_filter(c,filter);pthread_mutex_lock(&graph_lock);
        n=--g->refs;
    }
    if(g&&!n){media_call(g->id,2,NULL,0);for(int i=0;i<64;++i)if(graphs[i]==g)graphs[i]=NULL;for(int i=0;i<I_COUNT;++i)if(g->ifaces[i])gfree(g->ifaces[i]);free(g);}
    pthread_mutex_unlock(&graph_lock);RET(c,n,1);
}

static void TypeCount(CPU *c){if(ARG(c,1))P32(ARG(c,1),0);RET(c,0,2);}
static void ok0(CPU *c){RET(c,0,1);}static void ok1(CPU *c){RET(c,0,2);}static void ok3(CPU *c){RET(c,0,4);}
static void notimpl1(CPU *c){RET(c,0x80004001u,2);}
static uint32_t render_file(Graph *g,uint32_t name){
    char win[1024],path[2048];w2a(name,win,sizeof win);host_path(win,path,sizeof path);
    double result=media_call(g->id,3,path,0);g->reported=0;
    __android_log_print(result<0?ANDROID_LOG_ERROR:ANDROID_LOG_INFO,"FNVMedia","RenderFile(%s): %s",win,result<0?"decoder failed":"prepared");
    return result<0?0x80040218u:0;
}
static void GB_RenderFile(CPU *c){LOCK;uint32_t hr=!g?0x80004005u:ARG(c,2)?0x80004001u:render_file(g,ARG(c,1));DONE(hr,3);}
// The pinned executable's music source is a CAsyncReader wrapping a BSFile.
// Its constructor at 0x831e20 supplies the stream to the base at +0x80;
// BSFile's constructor at 0x82f010 copies the file name to stream+4.
// Support this verified source layout; other filters remain explicit failures.
static void GB_AddFilter(CPU *c) {
    uint32_t filter=ARG(c,1);
    if(!filter||G32(filter)!=0x107cd6c){RET(c,0x80004001u,3);return;}
    uint32_t stream=G32(filter+0x74);
    if(!stream||G32(stream)!=0x107cb4c){RET(c,0x80004001u,3);return;}
    char win[261],path[2048];memcpy(win,MEM+stream+4,260);win[260]=0;
    if(!memchr(MEM+stream+4,0,260)){RET(c,0x80070057u,3);return;}
    host_path(win,path,sizeof path);LOCK;
    if(!g||g->filter)DONE(0x80004001u,3);
    double loaded=media_call(g->id,3,path,0);
    __android_log_print(loaded<0?ANDROID_LOG_ERROR:ANDROID_LOG_INFO,"FNVMedia","Guest file source %s: %s",win,loaded<0?"decoder failed":"prepared");
    if(loaded<0)DONE(0x80040218u,3);
    g->filter=filter;g->reported=0;uint32_t graph_object=iface(g,I_GRAPH);
    pthread_mutex_unlock(&graph_lock);
    guest_method(c,filter,1,NULL,0);
    uint32_t args[]={graph_object,ARG(c,2)};
    Graph *previous_join=joining_graph;joining_graph=g;
    uint32_t result=guest_method(c,filter,13,args,2);joining_graph=previous_join;RET(c,result,3);
}
static void GB_Render(CPU *c){LOCK;uint32_t pin=ARG(c,1);int ready=g&&g->filter&&pin==g->filter+0xf8;DONE(ready?0:0x80004001u,2);}
static void GB_RemoveFilter(CPU *c){LOCK;uint32_t filter=ARG(c,1);if(!g||filter!=g->filter)DONE(0x80070057u,2);g->filter=0;media_call(g->id,6,NULL,0);pthread_mutex_unlock(&graph_lock);release_filter(c,filter);RET(c,0,2);}
static void MC_RenderFile(CPU *c){LOCK;uint32_t hr=g?render_file(g,ARG(c,1)):0x80004005u;DONE(hr,2);}
#define CONTROL(name,op) static void name(CPU *c){LOCK;double r=g?media_call(g->id,op,NULL,0):-1;if(g)g->reported=0;DONE(r<0?0x80040227u:0,1);}
CONTROL(MC_Run,4) CONTROL(MC_Pause,5) CONTROL(MC_Stop,6)
static void MC_GetState(CPU *c){LOCK;double v=g?media_call(g->id,7,NULL,0):-1;P32(ARG(c,2),v<0?0:(uint32_t)v);DONE(v<0?0x80040227u:0,3);}
static void ME_GetEvent(CPU *c){LOCK;
    if(g&&g->event_count){uint32_t *event=g->events[g->event_head];P32(ARG(c,1),event[0]);P32(ARG(c,2),event[1]);P32(ARG(c,3),event[2]);g->event_head=(g->event_head+1)%32;--g->event_count;DONE(0,5);}
if(g&&!g->cancel_complete&&!g->reported&&media_call(g->id,13,NULL,0)==1){g->reported=1;P32(ARG(c,1),1);P32(ARG(c,2),0);P32(ARG(c,3),0);DONE(0,5);}DONE(0x80004004u,5);}
static void Sink_Notify(CPU *c){LOCK;if(!g)DONE(0x80004005u,4);if(g->event_count==32)DONE(0x8007000eu,4);uint32_t *event=g->events[(g->event_head+g->event_count++)%32];event[0]=ARG(c,1);event[1]=ARG(c,2);event[2]=ARG(c,3);DONE(0,4);}
static void ME_WaitForCompletion(CPU *c){LOCK;int complete=g&&media_call(g->id,13,NULL,0)==1;P32(ARG(c,2),complete?1:0);DONE(complete?0:0x80040213u,3);}
static void ME_Cancel(CPU *c){LOCK;if(!g||ARG(c,1)!=1)DONE(0x80070057u,2);g->cancel_complete=1;DONE(0,2);}
static void ME_Restore(CPU *c){LOCK;if(!g||ARG(c,1)!=1)DONE(0x80070057u,2);g->cancel_complete=0;DONE(0,2);}
static void BA_put_Volume(CPU *c){LOCK;int v=(int32_t)ARG(c,1);if(!g||v< -10000||v>0)DONE(0x80070057u,2);g->volume=v;double r=media_call(g->id,8,NULL,v);DONE(r<0?0x80004005u:0,2);}
static void BA_get_Volume(CPU *c){LOCK;P32(ARG(c,1),g?g->volume:0);DONE(g?0:0x80004005u,2);}
static void BA_put_Balance(CPU *c){LOCK;int v=(int32_t)ARG(c,1);if(!g||v< -10000||v>10000)DONE(0x80070057u,2);g->balance=v;double r=media_call(g->id,9,NULL,v);DONE(r<0?0x80004005u:0,2);}
static void BA_get_Balance(CPU *c){LOCK;P32(ARG(c,1),g?g->balance:0);DONE(g?0:0x80004005u,2);}
static void put64(uint32_t p,int64_t v){memcpy(MEM+p,&v,8);}
static int64_t get64(uint32_t p){int64_t v;memcpy(&v,MEM+p,8);return v;}
#define TIME_GET(name,op) static void name(CPU *c){LOCK;double v=g?media_call(g->id,op,NULL,0):-1;if(v>=0)put64(ARG(c,1),(int64_t)(v*1e7));DONE(v<0?0x80040227u:0,2);}
TIME_GET(MS_GetDuration,10) TIME_GET(MS_GetCurrentPosition,11) TIME_GET(MS_GetStopPosition,10)
static void MS_GetPositions(CPU *c){LOCK;double cur=g?media_call(g->id,11,NULL,0):-1,dur=g?media_call(g->id,10,NULL,0):-1;if(cur<0||dur<0)DONE(0x80040227u,3);if(ARG(c,1))put64(ARG(c,1),(int64_t)(cur*1e7));if(ARG(c,2))put64(ARG(c,2),(int64_t)(dur*1e7));DONE(0,3);}
static void MS_SetPositions(CPU *c){LOCK;if(!g)DONE(0x80004005u,5);if(ARG(c,3)&&(ARG(c,4)&3))DONE(0x80004001u,5);int flags=ARG(c,2)&3;double target=0;if(ARG(c,1)&&flags){target=get64(ARG(c,1))/1e7;if(flags!=1){double cur=media_call(g->id,11,NULL,0);if(cur<0)DONE(0x80040227u,5);target+=cur;}if(media_call(g->id,12,NULL,target)<0)DONE(0x80040227u,5);g->reported=0;if(ARG(c,2)&8)put64(ARG(c,1),(int64_t)(target*1e7));}DONE(0,5);}
static const uint8_t time_format[]={0x70,0xa6,0x95,0x7b,0xc5,0x84,0xd0,0x11,0xa1,0x18,0,0xa0,0xc9,5,0x5e,0x9e};
static void MS_GetTimeFormat(CPU *c){memcpy(MEM+ARG(c,1),time_format,16);RET(c,0,2);}
static void MS_IsFormat(CPU *c){RET(c,!memcmp(MEM+ARG(c,1),time_format,16)?0:1,2);}
static void MS_SetFormat(CPU *c){RET(c,!memcmp(MEM+ARG(c,1),time_format,16)?0:0x80070057u,2);}
static void MS_GetCaps(CPU *c){P32(ARG(c,1),0x3f);RET(c,0,2);}
static void MS_CheckCaps(CPU *c){uint32_t want=G32(ARG(c,1)),got=want&0x3f;P32(ARG(c,1),got);RET(c,want==got?0:got?1:0x80004005u,2);}
static void MS_GetRate(CPU *c){double v=1;memcpy(MEM+ARG(c,1),&v,8);RET(c,0,2);}
static void MS_SetRate(CPU *c){uint64_t bits=((uint64_t)ARG(c,2)<<32)|ARG(c,1);double v;memcpy(&v,&bits,8);RET(c,v==1?0:0x80004001u,3);}
static void MS_GetPreroll(CPU *c){put64(ARG(c,1),0);RET(c,0,2);}
#define DOUBLE_GET(name,op) static void name(CPU *c){LOCK;double v=g?media_call(g->id,op,NULL,0):-1;if(v>=0)memcpy(MEM+ARG(c,1),&v,8);DONE(v<0?0x80040227u:0,2);}
DOUBLE_GET(MP_get_Duration,10) DOUBLE_GET(MP_get_CurrentPosition,11)
static void MP_put_CurrentPosition(CPU *c){LOCK;uint64_t bits=((uint64_t)ARG(c,2)<<32)|ARG(c,1);double v;memcpy(&v,&bits,8);if(!g||!isfinite(v)||v<0)DONE(0x80070057u,3);double result=media_call(g->id,12,NULL,v);g->reported=0;DONE(result<0?0x80040227u:0,3);}
static void MP_CanSeek(CPU *c){P32(ARG(c,1),0xffffffffu);RET(c,0,2);}

#define M(n, f, a) {n, f, a}
#define UNK M("QueryInterface", DS_QI, 2), M("AddRef", DS_AddRef, 0), M("Release", DS_Release, 0)
#define DISP M("GetTypeInfoCount", TypeCount, 1), M("GetTypeInfo", NULL, 3), M("GetIDsOfNames", NULL, 5), M("Invoke", NULL, 8)
static const ComMethod graph_m[] = {UNK, M("AddFilter", GB_AddFilter, 2), M("RemoveFilter", GB_RemoveFilter, 1), M("EnumFilters", notimpl1, 1),
    M("FindFilterByName", NULL, 2), M("ConnectDirect", NULL, 3), M("Reconnect", NULL, 1), M("Disconnect", NULL, 1),
    M("SetDefaultSyncSource", ok0, 0), M("Connect", NULL, 2), M("Render", GB_Render, 1), M("RenderFile", GB_RenderFile, 2),
    M("AddSourceFilter", NULL, 3), M("SetLogFile", NULL, 1), M("Abort", NULL, 0), M("ShouldOperationContinue", ok0, 0), M("AddSourceFilterForMoniker", NULL, 4), M("ReconnectEx", NULL, 2), M("RenderEx", NULL, 3)};
static const ComMethod control_m[] = {UNK, DISP, M("Run", MC_Run, 0), M("Pause", MC_Pause, 0), M("Stop", MC_Stop, 0),
    M("GetState", MC_GetState, 2), M("RenderFile", MC_RenderFile, 1), M("AddSourceFilter", NULL, 2), M("get_FilterCollection", NULL, 1),
    M("get_RegFilterCollection", NULL, 1), M("StopWhenReady", MC_Stop, 0)};
static const ComMethod event_m[] = {UNK, DISP, M("GetEventHandle", NULL, 1), M("GetEvent", ME_GetEvent, 4),
    M("WaitForCompletion", ME_WaitForCompletion, 2), M("CancelDefaultHandling", ME_Cancel, 1), M("RestoreDefaultHandling", ME_Restore, 1),
    M("FreeEventParams", ok3, 3), M("SetNotifyWindow", NULL, 3), M("SetNotifyFlags", NULL, 1), M("GetNotifyFlags", NULL, 1)};
static const ComMethod audio_m[] = {UNK, DISP, M("put_Volume", BA_put_Volume, 1), M("get_Volume", BA_get_Volume, 1),
    M("put_Balance", BA_put_Balance, 1), M("get_Balance", BA_get_Balance, 1)};
static const ComMethod seeking_m[] = {UNK, M("GetCapabilities", MS_GetCaps, 1), M("CheckCapabilities", MS_CheckCaps, 1),
    M("IsFormatSupported", MS_IsFormat, 1), M("QueryPreferredFormat", MS_GetTimeFormat, 1), M("GetTimeFormat", MS_GetTimeFormat, 1),
    M("IsUsingTimeFormat", MS_IsFormat, 1), M("SetTimeFormat", MS_SetFormat, 1), M("GetDuration", MS_GetDuration, 1),
    M("GetStopPosition", MS_GetStopPosition, 1), M("GetCurrentPosition", MS_GetCurrentPosition, 1),
    M("ConvertTimeFormat", NULL, 5), M("SetPositions", MS_SetPositions, 4), M("GetPositions", MS_GetPositions, 2),
    M("GetAvailable", NULL, 2), M("SetRate", MS_SetRate, 2), M("GetRate", MS_GetRate, 1), M("GetPreroll", MS_GetPreroll, 1)};
static const ComMethod position_m[] = {UNK, DISP, M("get_Duration", MP_get_Duration, 1), M("put_CurrentPosition", MP_put_CurrentPosition, 2),
    M("get_CurrentPosition", MP_get_CurrentPosition, 1), M("get_StopTime", MP_get_Duration, 1), M("put_StopTime", NULL, 2),
    M("get_PrerollTime", MS_GetPreroll, 1), M("put_PrerollTime", NULL, 2), M("put_Rate", MS_SetRate, 2), M("get_Rate", MS_GetRate, 1),
    M("CanSeekForward", MP_CanSeek, 1), M("CanSeekBackward", MP_CanSeek, 1)};


static const ComMethod sink_m[]={UNK,M("Notify",Sink_Notify,3)};
uint32_t dshow_create_filtergraph(uint32_t iid) {
    int selected=which(iid);if(selected<0)return 0;
    pthread_mutex_lock(&graph_lock);
    if(!vt[0]){
#define V(i,m) vt[i]=com_vtable(#m,m,(int)(sizeof m/sizeof *m))
        V(I_GRAPH,graph_m);V(I_CONTROL,control_m);V(I_EVENT,event_m);V(I_AUDIO,audio_m);V(I_SEEKING,seeking_m);V(I_POSITION,position_m);V(I_SINK,sink_m);
#undef V
    }
    int slot=-1;for(int i=0;i<64;++i)if(!graphs[i]){slot=i;break;}
    if(slot<0){pthread_mutex_unlock(&graph_lock);return 0;}
    Graph *g=calloc(1,sizeof *g);if(!g){pthread_mutex_unlock(&graph_lock);return 0;}
    g->id=next_id++;g->refs=1;
    if(media_call(g->id,1,NULL,0)<0){free(g);pthread_mutex_unlock(&graph_lock);return 0;}
    graphs[slot]=g;uint32_t object=iface(g,selected);
    pthread_mutex_unlock(&graph_lock);return object;
}
void host_media_reset(void) {
    // Guest workers have joined. Release Java decoders before guest memory resets.
    pthread_mutex_lock(&graph_lock);media_call(0,0,NULL,0);
    for(int i=0;i<64;++i)if(graphs[i]){free(graphs[i]);graphs[i]=NULL;}
    memset(vt,0,sizeof vt);joining_graph=NULL;filter_release_depth=0;pthread_mutex_unlock(&graph_lock);
}
static int cycle_result;
static void cycle_join(CPU *c){uint32_t object=ARG(c,0),sink=G32(object+8);if(sink){P32(object+8,0);guest_method(c,sink,2,NULL,0);}P32(object+12,G32(object+12)+1);RET(c,0,3);}
static void cycle_release(CPU *c){uint32_t object=ARG(c,0),refs=G32(object+4);P32(object+4,refs-1);RET(c,refs-1,1);}
static void cycle_fixture(CPU *cpu){
    (void)cpu;uint32_t iid=gmalloc(16),out=gmalloc(4),filter=gmalloc(16),vtable=gmalloc(56);
    P32(iid,0x56a868a9);uint32_t object=dshow_create_filtergraph(iid);
    if(!object){cycle_result=1;return;}
    P32(vtable+13*4,host_register_import("test","MusicCycleJoin",cycle_join));
    P32(vtable+2*4,host_register_import("test","MusicCycleRelease",cycle_release));
    P32(filter,vtable);P32(filter+4,1);
    pthread_mutex_lock(&graph_lock);Graph *g=NULL;for(int i=0;i<64;++i)if(graphs[i]&&graphs[i]->ifaces[I_GRAPH]==object)g=graphs[i];
    if(g)g->filter=filter;pthread_mutex_unlock(&graph_lock);
    if(!g){cycle_result=2;return;}
    joining_graph=g;P32(iid,0x56a868a2);uint32_t query[]={object,iid,out};
    uint32_t hr=host_shim_test_call(DS_QI,query,3);joining_graph=NULL;
    if(hr){cycle_result=3;return;}P32(filter+8,G32(out));
    if(host_shim_test_call(DS_Release,&object,1)||G32(filter+4)||G32(filter+8)||G32(filter+12)!=1){cycle_result=4;return;}
    gfree(iid);gfree(out);gfree(filter);gfree(vtable);cycle_result=0;
}
int host_media_com_selftest(void) {
    uint32_t iid=gmalloc(32),out=gmalloc(4);P32(iid,0x56a868a9);
    uint32_t object=dshow_create_filtergraph(iid);if(!object)return 1;
    P32(iid,0x56a868b3);uint32_t query[]={object,iid,out};
    if(host_shim_test_call(DS_QI,query,3))return 2;
    uint32_t audio=G32(out),set[]={audio,(uint32_t)-5000},get[]={audio,out};
    if(host_shim_test_call(BA_put_Volume,set,2)||host_shim_test_call(BA_get_Volume,get,2)||G32(out)!=(uint32_t)-5000)return 3;
    P32(iid,0x56a868a2);
    if(host_shim_test_call(DS_QI,query,3))return 5;
    uint32_t sink=G32(out),notify[]={sink,42,123,456};
    if(host_shim_test_call(Sink_Notify,notify,4))return 6;
    uint32_t event[]={object,iid,iid+4,iid+8,0};
    if(host_shim_test_call(ME_GetEvent,event,5)||G32(iid)!=42||G32(iid+4)!=123||G32(iid+8)!=456)return 7;
    if(host_shim_test_call(DS_Release,&sink,1)!=2)return 8;
    if(host_shim_test_call(DS_Release,&audio,1)!=1||host_shim_test_call(DS_Release,&object,1)!=0)return 4;
    gfree(iid);gfree(out);CPU cpu={0};cycle_result=9;
    int trap=rt_run_callback(&cpu,cycle_fixture,10000);return trap?30+trap:cycle_result?40+cycle_result:0;
}
#endif
