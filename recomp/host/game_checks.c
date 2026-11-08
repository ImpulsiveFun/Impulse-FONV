#ifdef __ANDROID__
#include "host.h"

static int getter_error;
static void test_luck(CPU *cpu) {
    uint32_t actor=REG(cpu,R_ECX);
    if(ARG(cpu,0)!=11)getter_error=1;
    float value;memcpy(&value,MEM+actor+4,4);
    double result=value;
    // Return one x87 value using the shared generated-code register layout.
    memmove(cpu->r+4368,cpu->r+4352,7*16);
    memcpy(cpu->r+4352,&result,sizeof result);TOPADJ(cpu->r,-1);
    RET(cpu,0,1);
}
int host_actor_value_selftest(void) {
    // Execute the real CritChance callback and its real actor-value scaling.
    // Only the actor's virtual Luck getter is a fixture. Restore image globals
    // before normal startup so this test cannot change the player's settings.
    const uint32_t base_setting=0x11cdee4u,mult_setting=0x11cdb4cu;
    const uint32_t luck_slot=0x11d61c8u+11*4;
    uint32_t saved_base=G32(base_setting),saved_mult=G32(mult_setting),saved_info=G32(luck_slot);
    uint32_t memory=gmalloc(2048);if(!memory)return 1;
    memset(MEM+memory,0,2048);
    uint32_t actor=memory,vtable=memory+32,info=memory+64,stack=memory+1800;
    P32(actor,vtable);P32(vtable+12,host_register_import("test","ActorLuck",test_luck));
    P32(luck_slot,info);
    float base=2.25f,mult=1.5f;memcpy(MEM+base_setting,&base,4);memcpy(MEM+mult_setting,&mult,4);
    const float values[]={-3.0f,0.0f,7.5f,20.0f};int failure=0;
    for(unsigned clamp=0;clamp<2&&!failure;++clamp){
        P32(info+0x44,clamp?0x8008u:0u);
        for(unsigned i=0;i<sizeof values/sizeof *values;++i){
            CPU cpu={0};SETREG(&cpu,R_ESP,stack);ST(cpu.r+4256,0x027f,2);
            P32(stack,HOST_EXIT_THUNK);P32(stack+4,actor);P32(stack+8,14);
            memcpy(MEM+actor+4,&values[i],4);getter_error=0;
            int trap=rt_run_entry(&cpu,0x643b10u,100000);
            double returned;memcpy(&returned,cpu.r+4352,sizeof returned);
            float luck=clamp?fmaxf(1.0f,fminf(10.0f,values[i])):values[i];
            double expected=(float)(base+(double)mult*luck);
            if(trap||getter_error||REG(&cpu,R_ESP)!=stack+4||cpu.r[REG_TOP]!=7||returned!=expected){failure=2+(int)(clamp*4+i);break;}
        }
    }
    P32(base_setting,saved_base);P32(mult_setting,saved_mult);P32(luck_slot,saved_info);gfree(memory);
    return failure;
}
#endif
