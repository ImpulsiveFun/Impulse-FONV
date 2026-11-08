#ifdef __ANDROID__
// Bink configuration and explicit open failure, matching the supplied macOS
// scaffold. There is no Bink decoder here; callers can take their normal
// optional-movie failure path without receiving a fabricated movie handle.
#include <android/log.h>
#include "shims.h"
static uint32_t sound_factory, sound_argument, error_string;
static void BinkSetSoundSystem_(CPU *c) {
    sound_factory=ARG(c,0);sound_argument=ARG(c,1);
    RET(c,sound_factory?1:0,2);
}
static void BinkOpenDirectSound_(CPU *c) { RET(c,0,1); }
static void BinkOpen_(CPU *c) {
    __android_log_print(ANDROID_LOG_INFO,"FNVMedia","Movie not opened (Bink decoder pending): %.180s",GSTR(ARG(c,0))?GSTR(ARG(c,0)):"(null)");
    RET(c,0,2);
}
static void BinkGetError_(CPU *c) {
    if(!error_string){
        const char *text="Bink video decoding is not implemented in this Android development build.";
        error_string=gmalloc((uint32_t)strlen(text)+1);memcpy(MEM+error_string,text,strlen(text)+1);
    }
    RET(c,error_string,0);
}
static void BinkClose_(CPU *c) {
    if(ARG(c,0))rt_trap(c,TRAP_IMPORT,host_current_import_address);
    RET(c,0,1);
}
void host_video_reset(void){sound_factory=sound_argument=error_string=0;}
const ShimTable android_video_shims={(const Shim[]){
    {"_BinkSetSoundSystem@8",BinkSetSoundSystem_},{"_BinkOpenDirectSound@4",BinkOpenDirectSound_},
    {"_BinkOpen@8",BinkOpen_},{"_BinkGetError@0",BinkGetError_},{"_BinkClose@4",BinkClose_}
},5};
#endif
