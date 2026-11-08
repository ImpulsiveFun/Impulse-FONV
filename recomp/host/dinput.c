// DirectInput8 keyboard and mouse devices consume the shared platform input queue.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include "com.h"
#include "input.h"
#ifdef __ANDROID__
#include <android/log.h>
#include <stdatomic.h>
#endif

static uint32_t vt_di8, vt_dev;
static pthread_mutex_t dinput_lock = PTHREAD_MUTEX_INITIALIZER;

static void QueryInterface_(CPU *c) {
    static const uint8_t unknown[16] = {0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    uint32_t object = ARG(c, 0), iid = ARG(c, 1), output = ARG(c, 2);
    if (!output) { RET(c, 0x80004003u, 3); return; }
    if (iid && (!memcmp(MEM + iid, unknown, sizeof unknown) ||
        (G32(object) == vt_di8 && !memcmp(MEM + iid, MEM + COM_DATA(object), 16)))) {
        __atomic_add_fetch((uint32_t *)(void *)(MEM + object + 4), 1, __ATOMIC_SEQ_CST);
        P32(output, object); RET(c, 0, 3); return;
    }
    P32(output, 0); RET(c, 0x80004002u, 3);
}

// ---- IDirectInputDevice8A ---------------------------------------------------------------------
static void Dev_GetCapabilities(CPU *c) { uint32_t p = ARG(c, 1); uint32_t sz = G32(p); memset(MEM + p + 4, 0, sz > 4 ? sz - 4 : 0); RET(c, 0, 2); }
// device kind lives in the COM data word: 0 keyboard, 1 mouse
static int dev_kind(CPU *c) { return (int)G32(COM_DATA(COM_THIS(c))); }
static uint64_t n_state[2], n_data[2], n_delivered[2];
static int trace_in(void) { static int t = -1; if (t < 0) t = getenv("FNV_TRACE_INPUT") != NULL; return t; }
static void report(void) {
    static uint64_t last; uint64_t now = host_uptime_ns();
    if (trace_in() && now - last > 2000000000ull) { last = now;
        fprintf(stderr, "[dinput] kbd state %llu data %llu delivered %llu | mouse state %llu data %llu delivered %llu\n",
            (unsigned long long)n_state[0], (unsigned long long)n_data[0], (unsigned long long)n_delivered[0],
            (unsigned long long)n_state[1], (unsigned long long)n_data[1], (unsigned long long)n_delivered[1]); }
}
static void Dev_GetDeviceState(CPU *c) {
    uint32_t n = ARG(c, 1), out = ARG(c, 2);
    n_state[dev_kind(c) & 1]++; report();
    memset(MEM + out, 0, n);
    if (dev_kind(c) == 0) { uint8_t k[256]; input_keyboard_state(k); memcpy(MEM + out, k, n < 256 ? n : 256); }
    else {
        int32_t dx, dy, dz; uint8_t b[8]; input_mouse_state(&dx, &dy, &dz, b);
#ifdef __ANDROID__
        static uint8_t last_button;
        if(dx||dy||dz||last_button!=b[0])__android_log_print(ANDROID_LOG_INFO,"FNVInput","Mouse state dx=%d dy=%d button=%u",dx,dy,b[0]);
        last_button=b[0];
#endif
        if (n >= 12) { P32(out, (uint32_t)dx); P32(out + 4, (uint32_t)dy); P32(out + 8, (uint32_t)dz); }
        memcpy(MEM + out + 12, b, n >= 20 ? 8 : n > 12 ? n - 12 : 0);    // DIMOUSESTATE (4 buttons) / DIMOUSESTATE2 (8)
    }
    RET(c, 0, 3);
}
// GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags): DIDEVICEOBJECTDATA = ofs, data, time, seq, appdata
static void Dev_GetDeviceData(CPU *c) {
    uint32_t sz = ARG(c, 1), arr = ARG(c, 2), inout = ARG(c, 3), flags = ARG(c, 4), cap = G32(inout), n = 0;
    int dev = dev_kind(c), peek = (flags & 1) != 0;
    static uint32_t seq;
    n_data[dev & 1]++; report();
    if (!arr) { P32(inout, (uint32_t)input_pending(dev)); if (!peek) { uint32_t o, d; while (input_pop_event(dev, &o, &d, -1)) {} } RET(c, 0, 5); return; }
    for (; n < cap; n++) {
        uint32_t o, d;
        if (!input_pop_event(dev, &o, &d, peek ? (int)n : -1)) break;
        uint32_t e = arr + n * sz;
        memset(MEM + e, 0, sz);
        P32(e, o); P32(e + 4, d); P32(e + 8, (uint32_t)(host_uptime_ns() / 1000000)); P32(e + 12, ++seq);
    }
    P32(inout, n);
    n_delivered[dev & 1] += n;
    if (n && trace_in()) fprintf(stderr, "[dinput] dev %d delivered %u event(s), first ofs %#x data %#x\n", dev, n, G32(arr), G32(arr + 4));
    RET(c, 0, 5);
}
COM_STUB_OK(Dev_SetDataFormat, 1)
COM_STUB_OK(Dev_SetCooperativeLevel, 2)
COM_STUB_OK(Dev_SetProperty, 2)
COM_STUB_OK(Dev_GetProperty, 2)
COM_STUB_OK(Dev_Acquire, 0)
COM_STUB_OK(Dev_Unacquire, 0)
COM_STUB_OK(Dev_SetEventNotification, 1)
COM_STUB_OK(Dev_Poll, 0)
COM_STUB_OK(Dev_EnumObjects, 3)
COM_STUB_OK(Dev_GetObjectInfo, 3)
COM_STUB_OK(Dev_GetDeviceInfo, 1)
COM_STUB_OK(Dev_RunControlPanel, 2)
COM_STUB_OK(Dev_Initialize, 3)

static const ComMethod dev_methods[] = {
    {"QueryInterface", QueryInterface_, 2}, {"AddRef", com_AddRef, 0}, {"Release", com_Release, 0},
    {"GetCapabilities", Dev_GetCapabilities, 1}, {"EnumObjects", Dev_EnumObjects, 3}, {"GetProperty", Dev_GetProperty, 2},
    {"SetProperty", Dev_SetProperty, 2}, {"Acquire", Dev_Acquire, 0}, {"Unacquire", Dev_Unacquire, 0},
    {"GetDeviceState", Dev_GetDeviceState, 2}, {"GetDeviceData", Dev_GetDeviceData, 4},
    {"SetDataFormat", Dev_SetDataFormat, 1}, {"SetEventNotification", Dev_SetEventNotification, 1},
    {"SetCooperativeLevel", Dev_SetCooperativeLevel, 2}, {"GetObjectInfo", Dev_GetObjectInfo, 3},
    {"GetDeviceInfo", Dev_GetDeviceInfo, 1}, {"RunControlPanel", Dev_RunControlPanel, 2},
    {"Initialize", Dev_Initialize, 3}, {"CreateEffect", NULL, 4}, {"EnumEffects", NULL, 3},
    {"GetEffectInfo", NULL, 2}, {"GetForceFeedbackState", NULL, 1}, {"SendForceFeedbackCommand", NULL, 1},
    {"EnumCreatedEffectObjects", NULL, 3}, {"Escape", NULL, 1}, {"Poll", Dev_Poll, 0},
    {"SendDeviceData", NULL, 4}, {"EnumEffectsInFile", NULL, 4}, {"WriteEffectToFile", NULL, 4},
    {"BuildActionMap", NULL, 3}, {"SetActionMap", NULL, 3}, {"GetImageInfo", NULL, 1},
};

// ---- IDirectInput8A ---------------------------------------------------------------------------
static void DI_CreateDevice(CPU *c) {
    pthread_mutex_lock(&dinput_lock);
    if (!vt_dev) vt_dev = com_vtable("IDirectInputDevice8A", dev_methods, (int)(sizeof dev_methods / sizeof *dev_methods));
    pthread_mutex_unlock(&dinput_lock);
    uint32_t guid = ARG(c, 1);
    fprintf(stderr, "[dinput] CreateDevice guid %08x-...\n", G32(guid));
    uint32_t d = com_new(vt_dev, 16);
    P32(COM_DATA(d), G32(guid) == 0x6f1d2b60u ? 1u : 0u);      // GUID_SysMouse / GUID_SysKeyboard
    P32(ARG(c, 2), d);
    RET(c, 0, 4);
}
static void DI_EnumDevices(CPU *c) { RET(c, 0, 5); }      // nothing to enumerate (no gamepads yet)
COM_STUB_OK(DI_GetDeviceStatus, 1)
COM_STUB_OK(DI_RunControlPanel, 2)
COM_STUB_OK(DI_Initialize, 2)

static const ComMethod di8_methods[] = {
    {"QueryInterface", QueryInterface_, 2}, {"AddRef", com_AddRef, 0}, {"Release", com_Release, 0},
    {"CreateDevice", DI_CreateDevice, 3}, {"EnumDevices", DI_EnumDevices, 4}, {"GetDeviceStatus", DI_GetDeviceStatus, 1},
    {"RunControlPanel", DI_RunControlPanel, 2}, {"Initialize", DI_Initialize, 2}, {"FindDevice", NULL, 3},
    {"EnumDevicesBySemantics", NULL, 5}, {"ConfigureDevices", NULL, 4},
};

static void DirectInput8Create_(CPU *c) {
    if (!ARG(c, 3)) { RET(c, 0x80004003u, 5); return; }
    pthread_mutex_lock(&dinput_lock);
    if (!vt_di8) vt_di8 = com_vtable("IDirectInput8A", di8_methods, (int)(sizeof di8_methods / sizeof *di8_methods));
    pthread_mutex_unlock(&dinput_lock);
    uint32_t object = com_new(vt_di8, 16);
    if (ARG(c, 2)) memcpy(MEM + COM_DATA(object), MEM + ARG(c, 2), 16);
    P32(ARG(c, 3), object);
    RET(c, 0, 5);
}

const ShimTable dinput_shims = { (const Shim[]){ {"DirectInput8Create", DirectInput8Create_} }, 1 };

// ---- XInput 1.3 -----------------------------------------------------------------------------
#ifdef __ANDROID__
static _Atomic int xinput_reported;

static int xinput_output_valid(uint32_t address, size_t length) {
    return address >= 0x10000u && (uint64_t)address + length <= UINT64_C(0x100000000);
}

static void XInputGetState_(CPU *c) {
    uint32_t packet, output = ARG(c, 1);
    InputGamepad pad;
    if (ARG(c, 0) || !input_gamepad_state(&packet, &pad)) { RET(c, 1167, 2); return; }
    if (!xinput_output_valid(output, 4 + sizeof pad)) { RET(c, 87, 2); return; }
    P32(output, packet);
    memcpy(MEM + output + 4, &pad, sizeof pad);
    if (!atomic_load_explicit(&xinput_reported, memory_order_relaxed) &&
        !atomic_exchange_explicit(&xinput_reported, 1, memory_order_relaxed))
        __android_log_print(ANDROID_LOG_INFO, "FNVInput", "Guest received connected XInput controller state.");
    RET(c, 0, 2);
}

static void XInputSetState_(CPU *c) {
    uint32_t packet;
    InputGamepad pad;
    if (ARG(c, 0) || !input_gamepad_state(&packet, &pad)) { RET(c, 1167, 2); return; }
    // This virtual device advertises no vibration motors.
    RET(c, xinput_output_valid(ARG(c, 1), 4) ? 0 : 87, 2);
}

static void XInputGetCapabilities_(CPU *c) {
    uint32_t packet, output = ARG(c, 2);
    InputGamepad pad;
    if (ARG(c, 0) || !input_gamepad_state(&packet, &pad)) { RET(c, 1167, 3); return; }
    if (!xinput_output_valid(output, 20)) { RET(c, 87, 3); return; }
    InputGamepad capabilities = {
        .buttons = 0xf3ff, .left_trigger = 255, .right_trigger = 255,
        .left_x = 32767, .left_y = 32767, .right_x = 32767, .right_y = 32767,
    };
    P32(output, 0x0101); // Gamepad type and subtype, no capability flags.
    memcpy(MEM + output + 4, &capabilities, sizeof capabilities);
    P32(output + 16, 0);
    RET(c, 0, 3);
}
#else
static void XInputGetState_(CPU *c) { RET(c, 1167 /* ERROR_DEVICE_NOT_CONNECTED */, 2); }
static void XInputSetState_(CPU *c) { RET(c, 1167, 2); }
static void XInputGetCapabilities_(CPU *c) { RET(c, 1167, 3); }
#endif
const ShimTable xinput_shims = { (const Shim[]){ {"xinput1_3#2", XInputGetState_}, {"xinput1_3#3", XInputSetState_},
    {"XInputGetState", XInputGetState_}, {"XInputSetState", XInputSetState_}, {"XInputGetCapabilities", XInputGetCapabilities_} }, 5 };

#ifdef __ANDROID__
int host_xinput_selftest(int connected, int expected_right_x) {
    uint32_t args[] = {0, 0x250600};
    uint32_t status = host_shim_test_call(XInputGetState_, args, 2);
    if (!connected) return status == 1167;
    if (status || G32(args[1] + 4) != 0x80001000 ||
        G32(args[1] + 8) != 0x40000000 || G32(args[1] + 12) != (uint16_t)expected_right_x) return 0;
    uint32_t packet = G32(args[1]);
    if (host_shim_test_call(XInputGetState_, args, 2) || G32(args[1]) != packet) return 0;
    uint32_t caps[] = {0, 1, args[1]};
    return !host_shim_test_call(XInputGetCapabilities_, caps, 3) &&
        G32(args[1]) == 0x0101 && G32(args[1] + 4) == 0xfffff3ff && G32(args[1] + 16) == 0;
}

void host_dinput_reset(void) {
    vt_di8 = vt_dev = 0;
    memset(n_state, 0, sizeof n_state); memset(n_data, 0, sizeof n_data); memset(n_delivered, 0, sizeof n_delivered);
    input_reset();
}
static int input_test_method(uint32_t object, int method, const uint32_t *arguments, int count) {
    CPU cpu = {0}; const uint32_t stack = 0x2fff1000;
    uint32_t entry = G32(G32(object) + method * 4);
    SETREG(&cpu, R_ESP, stack); P32(stack, HOST_EXIT_THUNK); P32(stack + 4, object);
    for (int i = 0; i < count; ++i) P32(stack + 8 + i * 4, arguments[i]);
    int trap = rt_run_entry(&cpu, entry, 1000);
    return !trap && REG(&cpu, R_EAX) == 0 && REG(&cpu, R_ESP) == stack + 8 + count * 4;
}
int host_dinput_selftest(void) {
    uint32_t create[] = {0, 0x800, 0x250000, 0x250020, 0};
    memset(MEM + 0x250000, 0, 0x1000);
    if (host_shim_test_call(DirectInput8Create_, create, 5)) return 1;
    uint32_t root = G32(create[3]), device_args[] = {0x250040, 0x250060, 0};
    P32(device_args[0], 0x6f1d2b61u);
    if (!input_test_method(root, 3, device_args, 3)) return 2;
    uint32_t keyboard = G32(device_args[1]), state[] = {256, 0x250100};
    input_key(0x11, 1);
    if (!input_test_method(keyboard, 9, state, 2) || MEM[state[1] + 0x11] != 0x80) return 3;
    input_key(0x11, 0);
    if (!input_test_method(keyboard, 9, state, 2) || MEM[state[1] + 0x11]) return 4;
    uint32_t events[] = {20, 0x250300, 0x250280, 0}; P32(events[2], 8);
    if (!input_test_method(keyboard, 10, events, 4) || G32(events[2]) != 2 ||
        G32(events[1]) != 0x11 || G32(events[1] + 4) != 0x80 || G32(events[1] + 24) != 0) return 5;
    P32(device_args[0], 0x6f1d2b60u);
    if (!input_test_method(root, 3, device_args, 3)) return 6;
    uint32_t mouse = G32(device_args[1]); state[0] = 20;
    input_mouse_move(7, -3); input_mouse_button(0, 1);
    if (!input_test_method(mouse, 9, state, 2) || G32(state[1]) != 7 || (int32_t)G32(state[1] + 4) != -3 || MEM[state[1] + 12] != 0x80) return 7;
    if (!input_test_method(mouse, 9, state, 2) || G32(state[1]) || G32(state[1] + 4)) return 8;
    uint32_t unknown[] = {0x250500, 0x250520}; MEM[unknown[0] + 8] = 0xc0; MEM[unknown[0] + 15] = 0x46;
    if (!input_test_method(keyboard, 0, unknown, 2) || G32(unknown[1]) != keyboard || G32(keyboard + 4) != 2) return 9;
    host_dinput_reset();
    return 0;
}
#endif
