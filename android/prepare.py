"""Validate a local game, then generate C for the Android translation probe."""
import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXE = "FalloutNV.exe"
ASSETS = {
    "Data/FalloutNV.esm": b"TES4",
    **{f"Data/Fallout - {name}.bsa": b"BSA\x00"
       for name in ("Meshes", "Misc", "Sound", "Textures", "Textures2", "Voices1")},
}


def validate(source, executable=None):
    with ExitStack() as stack:
        if source.is_dir():
            def open_file(name):
                return (source / name).open("rb")
        else:
            archive = stack.enter_context(zipfile.ZipFile(source))
            names = archive.namelist()
            if len(set(names)) != len(names):
                raise ValueError("The ZIP contains duplicate paths.")

            def open_file(name):
                return archive.open(name)

        for name, magic in ASSETS.items():
            with open_file(name) as asset:
                header = asset.read(24)
            if len(header) != 24 or not header.startswith(magic):
                raise ValueError(f"Unexpected asset header: {name}")

        with (executable.open("rb") if executable is not None else open_file(EXE)) as exe:
            binary = exe.read(32 * 1024 * 1024 + 1)
        if len(binary) > 32 * 1024 * 1024 or len(binary) < 64 or binary[:2] != b"MZ":
            raise ValueError("Expected a New Vegas-sized Windows executable.")
        pe, = struct.unpack_from("<I", binary, 60)
        if pe + 26 > len(binary) or binary[pe:pe + 6] != b"PE\0\0\x4c\x01":
            raise ValueError("Expected a 32-bit x86 Windows executable.")
        if binary[pe + 24:pe + 26] != b"\x0b\x01":
            raise ValueError("Expected a PE32 executable.")
        return binary


def executable_sections(binary):
    pe, = struct.unpack_from("<I", binary, 60)
    section_count, = struct.unpack_from("<H", binary, pe + 6)
    optional_size, = struct.unpack_from("<H", binary, pe + 20)
    image_base, = struct.unpack_from("<I", binary, pe + 52)
    section_table = pe + 24 + optional_size
    if optional_size < 96 or section_table + section_count * 40 > len(binary):
        raise ValueError("Invalid PE section table.")

    for offset in range(section_table, section_table + section_count * 40, 40):
        _, rva, size, raw = struct.unpack_from("<IIII", binary, offset + 8)
        flags, = struct.unpack_from("<I", binary, offset + 36)
        if flags & 0x20000000 and raw + size <= len(binary):
            yield image_base + rva, size, raw


def probe_entries(binary):
    """Find aligned MOV EAX, imm32; RET sequences bounded by INT3 padding."""
    entries = []
    for start, size, raw in executable_sections(binary):
        section = binary[raw:raw + size]
        for match in re.finditer(rb"(?<=\xcc)\xb8.{4}\xc3(?=\xcc)", section, re.DOTALL):
            address = start + match.start()
            if address % 16 == 0:
                entries.append(address)
                if len(entries) == 16:
                    return entries
    return entries


def expected_returns(binary, pcode):
    """Read expected EAX results directly from the original executable bytes."""
    sections = list(executable_sections(binary))
    expected = {}
    with pcode.open(encoding="utf-8") as dump:
        for line in dump:
            address = json.loads(line)["entry"]
            for start, size, raw in sections:
                displacement = address - start
                if 0 <= displacement <= size - 6:
                    code = binary[raw + displacement:raw + displacement + 6]
                    if code[0] == 0xB8 and code[5] == 0xC3:
                        expected[address] = int.from_bytes(code[1:5], "little")
                        break
            else:
                raise ValueError(f"Function at {address:#x} does not match MOV EAX, imm32; RET.")
    if not expected:
        raise ValueError("Ghidra did not find any supported retail probe functions.")
    return expected


def startup_metadata(binary):
    optional = struct.unpack_from("<I", binary, 60)[0] + 24
    entry_rva, = struct.unpack_from("<I", binary, optional + 16)
    base, = struct.unpack_from("<I", binary, optional + 28)
    image_size, headers = struct.unpack_from("<II", binary, optional + 56)
    if not 0 < headers <= len(binary) or not headers <= image_size <= 64 * 1024 * 1024:
        raise ValueError("Unexpected PE image size.")
    count, = struct.unpack_from("<H", binary, optional - 18)
    optional_size, = struct.unpack_from("<H", binary, optional - 4)
    table = optional + optional_size
    image = bytearray(image_size)
    image[:headers] = binary[:headers]
    segments = [(base, 0, headers)]
    for offset in range(table, table + count * 40, 40):
        virtual_size, rva, size, raw = struct.unpack_from("<IIII", binary, offset + 8)
        size = min(size, virtual_size or size)
        if rva + size > image_size or raw + size > len(binary):
            raise ValueError("PE section extends past the image or file.")
        if size:
            image[rva:rva + size] = binary[raw:raw + size]
            segments.append((base + rva, raw, size))
    if base + image_size >= 0x2fff0000 or entry_rva + 10 > image_size:
        raise ValueError("PE image overlaps the startup stack or has an invalid entry point.")
    if image[entry_rva] != 0xe8 or image[entry_rva + 5] != 0xe9:
        raise ValueError("Expected a CALL/JMP CRT startup entry point.")
    init = entry_rva + 5 + struct.unpack_from("<i", image, entry_rva + 1)[0]
    crt = entry_rva + 10 + struct.unpack_from("<i", image, entry_rva + 6)[0]
    entries = [base + rva for rva in (init, crt, entry_rva)]
    sections = list(executable_sections(binary))
    if any(not any(start <= addr < start + size for start, size, _ in sections) for addr in entries):
        raise ValueError("Startup target is outside executable sections.")

    def word(offset):
        return struct.unpack_from("<I", image, offset)[0]

    def name(offset):
        end = image.find(0, offset, offset + 256)
        if end < 0:
            raise ValueError("Invalid PE import name.")
        return image[offset:end].decode("ascii")

    imports = []
    descriptor, = struct.unpack_from("<I", binary, optional + 104)
    while descriptor and word(descriptor + 12):
        dll = name(word(descriptor + 12))
        iat = word(descriptor + 16)
        lookup = word(descriptor) or iat
        while symbol := word(lookup):
            if iat + 4 > image_size or len(imports) >= 4096:
                raise ValueError("Invalid PE import table.")
            label = f"#{symbol & 0xffff}" if symbol & 0x80000000 else name(symbol + 2)
            imports.append((base + iat, f"{dll}!{label}"))
            lookup += 4
            iat += 4
        descriptor += 20
    return entries, segments, imports, image_size


def generate(binary, headless, output, extra_entries=(), startup_group=False, code_pointers=False):
    if output.exists():
        raise ValueError("Choose a fresh generated output directory.")
    entries = probe_entries(binary)
    if not entries:
        raise ValueError("No supported probe sequences found in the supplied executable.")
    startup_entries, segments, imports, image_size = startup_metadata(binary)
    sections = list(executable_sections(binary))
    if any(not any(start <= addr < start + size for start, size, _ in sections)
           for addr in extra_entries):
        raise ValueError("An additional startup entry is outside executable sections.")
    with TemporaryDirectory(prefix="fnv-probe-", dir=ROOT / "android") as scratch:
        temp = Path(scratch)
        executable = temp / EXE
        executable.write_bytes(binary)
        pcode = temp / "leaf.jsonl"
        startup_pcode = temp / "startup.jsonl"
        root_file = temp / "startup-roots.txt"
        root_file.write_text("".join(f"{entry:x}\n" for entry in (*extra_entries, *startup_entries)), encoding="ascii")
        root_argument = "@" + str(root_file)
        group_scripts = (["-postScript", "DiscoverStartup.java",
                          root_argument, "callbacks" if code_pointers else "direct"]
                         if startup_group else [])
        subprocess.run([
            str(headless), str(temp), "fnv", "-import", str(executable),
            "-noanalysis", "-scriptPath", str(ROOT / "ghidra_scripts"),
            "-postScript", "SeedProbeFunctions.java", *(f"{entry:x}" for entry in entries),
            "-postScript", "DumpPcode.java", str(pcode), "constret", "16",
            "-postScript", "SeedProbeFunctions.java",
            root_argument,
            *group_scripts,
            *([] if startup_group else ["-postScript", "ResolveJumpTables.java"] * 3),
            "-postScript", "DumpPcode.java", str(startup_pcode), "all", "-deleteProject",
        ], check=True)
        expected = expected_returns(binary, pcode)
        if set(expected) != set(entries):
            raise ValueError("Ghidra did not export all selected probe entries.")
        generated = temp / "generated"
        subprocess.run([sys.executable, str(ROOT / "recomp" / "emit.py"),
                        str(startup_pcode), str(generated)], check=True)
        from nvse_translation import redirect_commands
        for unit in generated.glob("tu_*.c"):
            original = unit.read_text(encoding="utf-8")
            translated = redirect_commands(original)
            if translated != original:
                unit.write_text(translated, encoding="utf-8")
        table = (generated / "table.c").read_text()
        address_table = re.search(r"rt_fn_addr\[\]\s*=\s*\{([^}]+)\}", table)
        if address_table is None:
            raise ValueError("The emitter did not produce its function-address table.")
        emitted_entries = {int(value.strip().rstrip("uU"), 0)
                           for value in address_table[1].split(",") if value.strip()}
        for address in (*expected, *extra_entries, startup_entries[-1]):
            if address not in emitted_entries:
                raise ValueError(f"The emitter skipped the function at {address:#x}.")
        (generated / "probe_cases.h").write_text(
            "static const struct ProbeCase probe_cases[] = {\n"
            + "".join(f"    {{0x{address:x}u, 0x{eax:x}u}},\n"
                      for address, eax in sorted(expected.items()))
            + "};\n", encoding="ascii")
        (generated / "startup_image.h").write_text(
            f"#define STARTUP_ENTRY 0x{startup_entries[-1]:x}u\n"
            f'#define STARTUP_SHA256 "{hashlib.sha256(binary).hexdigest()}"\n'
            f"#define STARTUP_BASE {segments[0][0]}u\n"
            f"#define STARTUP_SIZE {image_size}u\n"
            "static const struct ImageSegment startup_segments[] = {\n"
            + "".join(f"    {{{addr}u, {raw}u, {size}u}},\n" for addr, raw, size in segments)
            + "};\nstatic const struct ImageImport startup_imports[] = {\n"
            + "".join(f"    {{{addr}u, {json.dumps(label)}}},\n" for addr, label in imports)
            + "};\n", encoding="ascii")
        output.parent.mkdir(parents=True, exist_ok=True)
        # The destination may be on a different volume.
        shutil.copytree(generated, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path, help="Local installation directory or ZIP")
    parser.add_argument("--exe", type=Path, help="Use a separate local FalloutNV executable")
    parser.add_argument("--ghidra", type=Path, help="Ghidra support/analyzeHeadless[.bat]")
    parser.add_argument("--output", type=Path, default=ROOT / "android" / "generated")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--startup-entry", type=lambda value: int(value, 0),
                        action="append", default=[], help="Additional startup function address")
    parser.add_argument("--startup-roots", type=Path,
                        help="Text file with one startup address per line and optional # comments")
    parser.add_argument("--startup-group", action="store_true",
                        help="Discover up to 131072 directly called startup function entries")
    parser.add_argument("--virtual-tables", action="store_true",
                        help="Include MSVC virtual methods validated through RTTI records")
    parser.add_argument("--code-pointers", action="store_true",
                        help="Include aligned/hotpatch callbacks at padding or decoded return boundaries")
    args = parser.parse_args()
    try:
        binary = validate(args.game, args.exe)
        print("Base asset headers and x86 executable format passed.")
        print(f"Executable SHA-256: {hashlib.sha256(binary).hexdigest()}")
        if not args.check_only:
            if args.ghidra is None or not args.ghidra.is_file():
                parser.error("Supply --ghidra with the path to analyzeHeadless.")
            entries = list(args.startup_entry)
            if args.virtual_tables:
                from scan_vtables import discover
                tables = discover(binary)
                methods = {method for _, group in tables.values() for method in group}
                if not tables:
                    raise ValueError("No RTTI-validated virtual tables found.")
                entries.extend(sorted(methods))
                print(f"Validated {len(tables)} virtual tables with {len(methods)} unique methods.")
            if args.startup_roots:
                for line in args.startup_roots.read_text(encoding="utf-8").splitlines():
                    value = line.split("#", 1)[0].strip()
                    if value:
                        entries.append(int(value, 0))
            generate(binary, args.ghidra.resolve(), args.output.resolve(),
                     list(dict.fromkeys(entries)), args.startup_group, args.code_pointers)
            print(f'Build with: gradle -p "{ROOT / "android"}" assembleDebug '
                  f'"-PfnvGenerated={args.output.resolve()}"')
    except (OSError, KeyError, ValueError, struct.error, zipfile.BadZipFile,
            subprocess.CalledProcessError) as error:
        parser.exit(1, f"Preparation failed: {error}\n")


if __name__ == "__main__":
    main()
