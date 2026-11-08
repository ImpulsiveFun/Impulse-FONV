"""Recover x86 MSVC virtual tables using their RTTI records, not pointer guesses."""
import struct
from prepare import executable_sections, startup_metadata


def discover(binary):
    _, segments, _, image_size = startup_metadata(binary)
    base = segments[0][0]
    image = bytearray(image_size)
    for address, raw, size in segments:
        image[address-base:address-base+size] = binary[raw:raw+size]
    code = list(executable_sections(binary))

    def mapped(address, size=4):
        return any(start <= address and address+size <= start+length
                   for start, _, length in segments)

    def word(address):
        if not mapped(address):
            raise ValueError('Unmapped RTTI pointer')
        return struct.unpack_from('<I', image, address-base)[0]

    def executable(address):
        return any(start <= address < start+size for start, size, _ in code)

    def string_word(address):
        # Game virtual methods are 16-byte aligned; unaligned CRT methods also
        # occur. Short ASCII/UTF-16 labels after a table can coincidentally fall
        # inside .text numerically. They are not method entries.
        data = struct.pack('<I', address)
        ascii_label = data[3] == 0 and all(32 <= value < 127 for value in data[:3])
        wide_label = data[1] == data[3] == 0 and 32 <= data[0] < 127 and 32 <= data[2] < 127
        return address % 16 != 0 and (ascii_label or wide_label)

    tables = {}
    pe = struct.unpack_from('<I', binary, 60)[0]
    count = struct.unpack_from('<H', binary, pe+6)[0]
    optional_size = struct.unpack_from('<H', binary, pe+20)[0]
    for index in range(count):
        section = pe+24+optional_size+40*index
        name = binary[section:section+8].rstrip(b'\0')
        if name != b'.rdata':
            continue
        _, rva, size, _ = struct.unpack_from('<IIII', binary, section+8)
        for slot in range(base+rva, base+rva+size-4, 4):
            try:
                locator = word(slot)
                if not mapped(locator, 20):
                    continue
                if word(locator) != 0 or word(locator+4) > 0x100000 or word(locator+8) > 0x100000:
                    continue
                descriptor, hierarchy = word(locator+12), word(locator+16)
                if not mapped(descriptor, 12) or not mapped(hierarchy, 16):
                    continue
                if word(hierarchy) != 0 or word(hierarchy+4) & ~7 or not 1 <= word(hierarchy+8) <= 4096:
                    continue
                if not mapped(word(hierarchy+12), word(hierarchy+8)*4):
                    continue
                name_start = descriptor+8-base
                name_end = image.find(b'\0', name_start, min(name_start+1024, image_size))
                if name_end < 0:
                    continue
                type_name = bytes(image[name_start:name_end])
                if not type_name.startswith((b'.?AV', b'.?AU')) or not type_name.endswith(b'@@'):
                    continue
                methods = []
                cursor = slot+4
                while len(methods) < 1024 and mapped(cursor) and executable(word(cursor)):
                    if string_word(word(cursor)):
                        break
                    methods.append(word(cursor))
                    cursor += 4
                if methods:
                    tables[slot+4] = (type_name.decode('ascii'), methods)
            except (ValueError, struct.error, UnicodeDecodeError):
                continue
    return tables


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    tables = discover(args.executable.read_bytes())
    roots = sorted({method for _, methods in tables.values() for method in methods})
    args.output.write_text(''.join(f'{address:#x}\n' for address in roots), encoding='ascii')
    print(f'Validated {len(tables)} virtual tables; {len(roots)} unique methods.')
    print(f'Current allocation stop included: {0x8225d0 in roots}')
