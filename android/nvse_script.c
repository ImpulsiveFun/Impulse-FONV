#include "../recomp/host/host.h"

// FalloutNV.exe 1.4.0.525 and xNVSE's 32-bit CommandInfo layout.
enum {
    SCRIPT_COMMANDS = 0x1190910,
    COMMAND_SIZE = 0x28,
    VANILLA_LIMIT = 0x1280,
    GET_BASE_OBJECT = 0x1403,
};
uint32_t fnv_script_commands = SCRIPT_COMMANDS;
uint32_t fnv_script_command_limit = VANILLA_LIMIT;

static void unsupported_script_command(CPU *cpu) {
    RET(cpu, 0, 0);
}

static void get_base_object(CPU *cpu) {
    uint32_t reference = ARG(cpu, 2);
    uint32_t result = ARG(cpu, 6);
    uint32_t form = reference ? G32(reference + 0x20) : 0;
    ST(MEM + result, form ? G32(form + 0x0c) : 0, 8);
    RET(cpu, 1, 0); // cdecl, the script runner cleans up the arguments.
}

int fnv_prepare_script_commands(void) {
    fnv_script_commands = SCRIPT_COMMANDS;
    fnv_script_command_limit = VANILLA_LIMIT;
    const uint32_t size = (GET_BASE_OBJECT + 2 - 0x1000) * COMMAND_SIZE;
    uint32_t table = gmalloc(size);
    static const char strings[] = "\0GetBaseObject\0Returns the base form of the calling reference.";
    uint32_t text = gmalloc(sizeof strings);
    uint32_t execute = host_register_import("xnvse-script", "GetBaseObject", get_base_object);
    uint32_t unsupported = host_register_import("xnvse-script", "UnsupportedCommand", unsupported_script_command);
    if (!table || !text || !execute || !unsupported) return 0;

    memset(MEM + table, 0, size);
    memcpy(MEM + table, MEM + SCRIPT_COMMANDS, (VANILLA_LIMIT - 0x1000) * COMMAND_SIZE);
    memcpy(MEM + text, strings, sizeof strings);
    for (uint32_t opcode = VANILLA_LIMIT; opcode <= GET_BASE_OBJECT + 1; ++opcode) {
        uint32_t command = table + (opcode - 0x1000) * COMMAND_SIZE;
        P32(command, text);
        P32(command + 4, text);
        P32(command + 8, opcode);
        P32(command + 12, text);
        P32(command + 24, unsupported);
    }
    uint32_t command = table + (GET_BASE_OBJECT - 0x1000) * COMMAND_SIZE;
    P32(command, text + 1);
    P32(command + 12, text + 15);
    P32(command + 16, 1); // Needs a calling reference, with zero parameters.
    P32(command + 24, execute);
    P32(command + 28, 0x005B1BA0); // Vanilla parser for commands without parameters.
    fnv_script_commands = table;
    fnv_script_command_limit = GET_BASE_OBJECT + 1;
    return 1;
}
