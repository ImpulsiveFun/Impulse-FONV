"""Build a tiny xNVSE-only quest mod with no game assets."""
import argparse
from pathlib import Path
import struct
import zipfile


def field(tag, data):
    return struct.pack("<4sH", tag, len(data)) + data


def string(tag, text):
    return field(tag, text.encode("ascii") + b"\0")


def record(tag, form_id, data):
    return struct.pack("<4sIIIIHH", tag, len(data), 0, form_id, 0, 15, 0) + data


def group(tag, entry):
    return struct.pack("<4sI4sIII", b"GRUP", len(entry) + 24, tag, 0, 0, 0) + entry


def operation(opcode, operands=b""):
    return struct.pack("<HH", opcode, len(operands)) + operands


def make_plugin():
    quest_id, script_id, message_id = 0x01000800, 0x01000801, 0x01000802
    source = (
        "scn AndroidBaseObjectCheckScript\r\n"
        "ref courierBase\r\n"
        "begin GameMode\r\n"
        "  set courierBase to Player.GetBaseObject\r\n"
        "  if courierBase == Player\r\n"
        "    ShowMessage AndroidBaseObjectCheckMessage\r\n"
        "  endif\r\n"
        "  StopQuest AndroidBaseObjectCheckQuest\r\n"
        "end\r\n"
    )
    # The local reference (SCRV) occupies reference slot 1. The following
    # SCRO entries are the player reference, base form, message and quest.
    base_object = b" r\x02\0X" + struct.pack("<HH", 0x1403, 0)
    condition = b" f\x01\0 Z\x03\0 =="
    body = (
        operation(0x15, b"f\x01\0" + struct.pack("<H", len(base_object)) + base_object)
        + operation(0x16, struct.pack("<HH", 1, len(condition)) + condition)
        + operation(0x1059, b"\x01\0r\x04\0" + bytes(6))
        + operation(0x19)
        + operation(0x1037, b"\x01\0r\x05\0")
    )
    bytecode = operation(0x1D) + operation(0x10, struct.pack("<HI", 0, len(body) + 4)) + body + operation(0x11)
    script = (
        string(b"EDID", "AndroidBaseObjectCheckScript")
        + field(b"SCHR", struct.pack("<IIIII", 0, 5, len(bytecode), 1, 0x10001))
        + field(b"SCDA", bytecode)
        + field(b"SCTX", source.encode("ascii"))
        + field(b"SLSD", struct.pack("<I", 1) + bytes(20))
        + string(b"SCVR", "courierbase")
        + field(b"SCRV", struct.pack("<I", 1))
        + b"".join(field(b"SCRO", struct.pack("<I", form_id))
                   for form_id in (0x14, 0x07, message_id, quest_id))
    )
    quest = (
        string(b"EDID", "AndroidBaseObjectCheckQuest")
        + field(b"SCRI", struct.pack("<I", script_id))
        + field(b"DATA", struct.pack("<BBBBf", 1, 50, 0, 0, 1.0))
    )
    message = (
        string(b"EDID", "AndroidBaseObjectCheckMessage")
        + string(b"DESC", "GetBaseObject returned the Courier's base form.")
        + string(b"FULL", "xNVSE script command passed")
        + field(b"INAM", bytes(4))
        + field(b"DNAM", struct.pack("<I", 1))
    )
    header = (
        field(b"HEDR", struct.pack("<fII", 1.34, 6, 0x803))
        + string(b"CNAM", "New Vegas Android test")
        + string(b"MAST", "FalloutNV.esm")
        + field(b"DATA", bytes(8))
    )
    return (
        record(b"TES4", 0, header)
        + group(b"SCPT", record(b"SCPT", script_id, script))
        + group(b"QUST", record(b"QUST", quest_id, quest))
        + group(b"MESG", record(b"MESG", message_id, message))
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Data/AndroidBaseObjectCheck.esp", make_plugin())
