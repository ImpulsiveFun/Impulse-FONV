#include <jni.h>
#include <stdio.h>
#include "rt.h"

extern uint8_t *rt_mem(void);
extern int rt_count(void);
extern uint32_t rt_addr(int i);
extern int rt_run(int i, CPU *cpu, int64_t budget);
extern uint32_t rt_trap_addr(void);

struct ProbeCase {
    uint32_t entry;
    uint32_t eax;
};
#include "probe_cases.h"

int host_math_trap_selftest(void) {
    int index = 0;
    while (index < rt_count() && rt_addr(index) != 0xed641au) ++index;
    if (index == rt_count()) return 0;
    CPU cpu = {0};
    ST(cpu.r + 16, 0x100000u, 4);
    int trap = rt_run(index, &cpu, 100);
    return trap == TRAP_SOFTINT && rt_trap_addr() == 0xed641du &&
           LD(cpu.r + 16, 4) == 0x10002cu;
}

int host_fmod_selftest(void) {
    const struct {double value,divisor,expected;} cases[]={{7.5,3,1.5},{-7.5,3,-1.5},{7.5,-3,1.5},{6,3,0}};
    uint32_t saved=(uint32_t)LD(MEM+0x126fd94,4);ST(MEM+0x126fd94,0,4);
    int index=0;while(index<rt_count()&&rt_addr(index)!=0xec9144)++index;
    if(index==rt_count()){ST(MEM+0x126fd94,saved,4);return 1;}
    int result=0;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i){
        CPU cpu={0};uint32_t stack=0x100000;
        ST(cpu.r+16,stack,4);ST(MEM+stack,0x0badf000,4);
        ST(cpu.r+4256,0x027f,2);ST(cpu.r+4258,0x400,2);
        cpu.r[REG_TOP]=6;STF(cpu.r+4352,cases[i].divisor);STF(cpu.r+4368,cases[i].value);
        int trap=rt_run(index,&cpu,1000);
        if(trap||LD(cpu.r+16,4)!=stack+4||LDF(cpu.r+4352)!=cases[i].expected||cpu.r[REG_TOP]!=7||(LD(cpu.r+4258,2)&0x400)){result=2+i;break;}
    }
    ST(MEM+0x126fd94,saved,4);return result;
}

int host_initializer_selftest(char *message, size_t capacity) {
    /* Independent checks of the FLD(float), FMUL(double), FSTP(float)
       instruction sequences, including their guest stack and x87 balance. */
    const struct { uint32_t entry, input, factor, output; } cases[] = {
        {0xf34eb0, 0x101143c, 0x1011588, 0x11c3188},
        {0xf34ed0, 0x101143c, 0x1011590, 0x11c3194},
        {0xf34f50, 0x10115a0, 0x1011588, 0x11c31a0},
        {0xf34f70, 0x10115a0, 0x1011590, 0x11c31ac},
    };
    const uint32_t stack = 0x100000;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        int index = 0;
        while (index < rt_count() && rt_addr(index) != cases[i].entry) ++index;
        if (index == rt_count()) {
            snprintf(message, capacity, "Initializer check entry %#x is missing.", cases[i].entry);
            return 0;
        }
        float input, expected;
        double factor;
        uint32_t saved, expected_bits;
        memcpy(&input, MEM + cases[i].input, sizeof input);
        memcpy(&factor, MEM + cases[i].factor, sizeof factor);
        expected = (float)((double)input * factor);
        memcpy(&expected_bits, &expected, sizeof expected_bits);
        memcpy(&saved, MEM + cases[i].output, sizeof saved);
        ST(MEM + cases[i].output, ~expected_bits, 4);
        CPU cpu = {0};
        ST(cpu.r + 16, stack, 4);
        ST(cpu.r + 20, 0x22334455u, 4);
        ST(cpu.r + 4256, 0x027fu, 2);
        ST(MEM + stack, 0x0badf000u, 4);
        int trap = rt_run(index, &cpu, 1000);
        uint32_t actual = (uint32_t)LD(MEM + cases[i].output, 4);
        ST(MEM + cases[i].output, saved, 4);
        if (trap || actual != expected_bits || LD(cpu.r + 16, 4) != stack + 4 ||
            LD(cpu.r + 20, 4) != 0x22334455u || cpu.r[REG_TOP] != 0) {
            snprintf(message, capacity, "Initializer check failed at %#x: trap %d, value %#x (expected %#x).",
                     cases[i].entry, trap, actual, expected_bits);
            return 0;
        }
    }
    return 1;
}

int host_memory_selftest(char *message, size_t capacity) {
    const uint32_t entry = 0xec44d0u, stack = 0x100000u, region = 0x200000u;
    int index = 0;
    while (index < rt_count() && rt_addr(index) != entry) ++index;
    if (index == rt_count()) {
        snprintf(message, capacity, "Memory-copy entry is missing.");
        return 0;
    }
    const unsigned lengths[] = {0, 1, 2, 3, 4, 7, 8, 15, 31, 32, 33, 65, 257};
    const unsigned destinations[] = {400, 8, 24};
    uint8_t expected[1024];
    for (size_t direction = 0; direction < 3; ++direction) {
        for (unsigned alignment = 0; alignment < 4; ++alignment) {
            for (size_t length = 0; length < sizeof lengths / sizeof lengths[0]; ++length) {
                for (size_t i = 0; i < sizeof expected; ++i)
                    expected[i] = (uint8_t)(i * 37 + 11);
                memcpy(MEM + region, expected, sizeof expected);
                unsigned source = 16 + alignment;
                unsigned destination = destinations[direction] + alignment;
                unsigned size = lengths[length];
                memmove(expected + destination, expected + source, size);
                CPU cpu = {0};
                ST(cpu.r + 12, 0x11223344u, 4);
                ST(cpu.r + 20, 0x22334455u, 4);
                ST(cpu.r + 24, 0x33445566u, 4);
                ST(cpu.r + 28, 0x44556677u, 4);
                ST(cpu.r + 16, stack, 4);
                ST(MEM + stack, 0x0badf000u, 4);
                ST(MEM + stack + 4, region + destination, 4);
                ST(MEM + stack + 8, region + source, 4);
                ST(MEM + stack + 12, size, 4);
                int trap = rt_run(index, &cpu, 50000);
                if (trap || (uint32_t)LD(cpu.r, 4) != region + destination ||
                    (uint32_t)LD(cpu.r + 16, 4) != stack + 4 ||
                    (uint32_t)LD(cpu.r + 12, 4) != 0x11223344u ||
                    (uint32_t)LD(cpu.r + 20, 4) != 0x22334455u ||
                    (uint32_t)LD(cpu.r + 24, 4) != 0x33445566u ||
                    (uint32_t)LD(cpu.r + 28, 4) != 0x44556677u ||
                    memcmp(MEM + region, expected, sizeof expected) != 0) {
                    snprintf(message, capacity,
                             "Memory-copy check failed: direction %zu, alignment %u, length %u, trap %d at %#x.",
                             direction, alignment, size, trap, rt_trap_addr());
                    return 0;
                }
            }
        }
    }
    return 1;
}

JNIEXPORT jstring JNICALL Java_org_fnvprobe_ProbeActivity_runProbe(JNIEnv *env, jclass cls) {
    (void)cls;
    enum { EAX = 0, ESP = 16 };
    const uint32_t stack = 0x100000;
    const size_t case_count = sizeof probe_cases / sizeof probe_cases[0];
    char message[256];
    int count = rt_count();
    uint8_t *memory = rt_mem();
    if (!memory) {
        return (*env)->NewStringUTF(env, "Probe failed: unable to map guest memory.");
    }

    for (size_t i = 0; i < case_count; ++i) {
        struct ProbeCase test = probe_cases[i];
        int index = 0;
        while (index < count && rt_addr(index) != test.entry) ++index;
        if (index == count) {
            snprintf(message, sizeof message, "Missing translated entry %#x.", test.entry);
            return (*env)->NewStringUTF(env, message);
        }
        CPU cpu = {0};
        ST(cpu.r + EAX, ~test.eax, 4);
        ST(cpu.r + ESP, stack, 4);
        ST(memory + stack, 0x0badf000u, 4);
        int trap = rt_run(index, &cpu, 100);
        uint32_t eax = (uint32_t)LD(cpu.r + EAX, 4);
        uint32_t esp = (uint32_t)LD(cpu.r + ESP, 4);
        if (trap || eax != test.eax || esp != stack + 4) {
            snprintf(message, sizeof message,
                     "FAIL at %#x: trap %d, EAX %#x (expected %#x), ESP %#x (expected %#x).",
                     test.entry, trap, eax, test.eax, esp, stack + 4);
            return (*env)->NewStringUTF(env, message);
        }
    }
    snprintf(message, sizeof message,
             "PASS: %zu translated New Vegas functions matched the original x86 instructions.\n"
             "This checks register returns and stack cleanup only.", case_count);
    return (*env)->NewStringUTF(env, message);
}
