"""Redirect pinned New Vegas script-command references in the AOT output.

The PC extender changes x86 instruction immediates at runtime. The Android
build uses these equivalent references when translating the same instructions.
"""

import re

TABLE = 0x1190910
LIMITS = {
    "593909": ("1280", "0x1000u"),
    "5aea57": ("1280", "0x1000u"),
    "5b115b": ("1280", "0x1000u"),
    "5b19a0": ("280", "0u"),
    "5bcbdd": ("280", "0u"),
    "681238": ("1280", "0x1000u"),
    "681255": ("280", "0u"),
    "681274": ("280", "0u"),
}
INSTRUCTION = re.compile(r"^L_([0-9a-f]+): /\*[^\n]*\*/\n(?:^[ \t]+[^\n]*\n)*", re.M)
ADDRESS = re.compile(r"\b0x(11909(?:1[048c]|2[0248c]|3[04])|1196d18)(?:ULL|u)\b")
DECLARATIONS = (
    'extern uint32_t fnv_script_commands;\n'
    'extern uint32_t fnv_script_command_limit;\n'
)


def redirect_commands(source):
    if DECLARATIONS in source or not any(key in source for key in ("11909", "1196d18", "L_593909:")):
        return source

    def replace_address(match):
        address = int(match[1], 16)
        if address == 0x1196D18:
            return "(fnv_script_commands + (fnv_script_command_limit - 0x1000u) * 0x28u + 8u)"
        return f"(fnv_script_commands + {address - TABLE:#x}u)"

    def instruction(match):
        label, body = match[1], match[0]
        comment, code = body.split("\n", 1)
        if label in LIMITS:
            old, base = LIMITS[label]
            code = code.replace(f"0x{old}ULL", f"(fnv_script_command_limit - 0x1000u + {base})")
        return comment + "\n" + ADDRESS.sub(replace_address, code)

    result = INSTRUCTION.sub(instruction, source)
    return result.replace('#include "rt.h"\n', '#include "rt.h"\n' + DECLARATIONS, 1) if result != source else source
