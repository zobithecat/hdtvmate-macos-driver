#!/usr/bin/env python3
"""
extract_firmware.py - Extract IT9300 firmware from liba3_phy_sony.so

The IT9300 USB bridge firmware is compiled directly into the Android .so
binary as static byte arrays:
  - brFirmware_codes
  - brFirmware_segments
  - brFirmware_partitions
  - brFirmware_scriptSets
  - brFirmware_scripts

This script:
1. Parses the ELF symbol table to find firmware data addresses
2. Extracts the raw bytes from the .rodata/.data sections
3. Generates a C header file (br_firmware.h) with the firmware data

Usage:
    python3 extract_firmware.py <path_to_liba3_phy_sony.so> [output_dir]
"""

import struct
import sys
import os
import subprocess
import re


def run_nm(so_path):
    """Run nm to get symbol addresses and sizes (including local symbols)."""
    # Use nm without -D to include local/static symbols
    result = subprocess.run(
        ['nm', '--print-size', so_path],
        capture_output=True, text=True
    )
    symbols = {}
    for line in result.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 4:
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
            sym_type = parts[2]
            name = parts[3]
            symbols[name] = {'addr': addr, 'size': size, 'type': sym_type}
        elif len(parts) >= 3:
            addr = int(parts[0], 16)
            sym_type = parts[1]
            name = parts[2]
            symbols[name] = {'addr': addr, 'size': 0, 'type': sym_type}
    # Also try -D for dynamic symbols
    result2 = subprocess.run(
        ['nm', '-D', '--print-size', so_path],
        capture_output=True, text=True
    )
    for line in result2.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 4:
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
            sym_type = parts[2]
            name = parts[3]
            if name not in symbols:
                symbols[name] = {'addr': addr, 'size': size, 'type': sym_type}
        elif len(parts) >= 3:
            addr = int(parts[0], 16)
            sym_type = parts[1]
            name = parts[2]
            if name not in symbols:
                symbols[name] = {'addr': addr, 'size': 0, 'type': sym_type}
    return symbols


def run_objdump_sections(so_path):
    """Get section offsets by parsing the ELF file directly."""
    import struct as st

    sections = {}
    with open(so_path, 'rb') as f:
        # ELF header
        ident = f.read(16)
        if ident[:4] != b'\x7fELF':
            print("Not an ELF file!")
            return sections

        ei_class = ident[4]  # 1=32bit, 2=64bit
        is_64 = (ei_class == 2)

        if is_64:
            f.seek(0)
            ehdr = f.read(64)
            e_shoff = st.unpack_from('<Q', ehdr, 40)[0]
            e_shentsize = st.unpack_from('<H', ehdr, 58)[0]
            e_shnum = st.unpack_from('<H', ehdr, 60)[0]
            e_shstrndx = st.unpack_from('<H', ehdr, 62)[0]
        else:
            f.seek(0)
            ehdr = f.read(52)
            e_shoff = st.unpack_from('<I', ehdr, 32)[0]
            e_shentsize = st.unpack_from('<H', ehdr, 46)[0]
            e_shnum = st.unpack_from('<H', ehdr, 48)[0]
            e_shstrndx = st.unpack_from('<H', ehdr, 50)[0]

        # Read section headers
        f.seek(e_shoff)
        shdrs = []
        for i in range(e_shnum):
            shdr_data = f.read(e_shentsize)
            if is_64:
                sh_name = st.unpack_from('<I', shdr_data, 0)[0]
                sh_type = st.unpack_from('<I', shdr_data, 4)[0]
                sh_addr = st.unpack_from('<Q', shdr_data, 16)[0]
                sh_offset = st.unpack_from('<Q', shdr_data, 24)[0]
                sh_size = st.unpack_from('<Q', shdr_data, 32)[0]
            else:
                sh_name = st.unpack_from('<I', shdr_data, 0)[0]
                sh_type = st.unpack_from('<I', shdr_data, 4)[0]
                sh_addr = st.unpack_from('<I', shdr_data, 12)[0]
                sh_offset = st.unpack_from('<I', shdr_data, 16)[0]
                sh_size = st.unpack_from('<I', shdr_data, 20)[0]
            shdrs.append({
                'name_idx': sh_name, 'type': sh_type,
                'addr': sh_addr, 'offset': sh_offset, 'size': sh_size
            })

        # Read string table
        if e_shstrndx < len(shdrs):
            strtab = shdrs[e_shstrndx]
            f.seek(strtab['offset'])
            strtab_data = f.read(strtab['size'])
        else:
            strtab_data = b''

        for shdr in shdrs:
            name_end = strtab_data.find(b'\x00', shdr['name_idx'])
            name = strtab_data[shdr['name_idx']:name_end].decode('ascii', errors='replace')
            if name:
                sections[name] = {
                    'size': shdr['size'],
                    'vma': shdr['addr'],
                    'file_offset': shdr['offset']
                }

    return sections


def find_firmware_symbols(symbols):
    """Find brFirmware_* symbols."""
    fw_syms = {}
    targets = [
        'brFirmware_codes',
        'brFirmware_segments',
        'brFirmware_partitions',
        'brFirmware_scriptSets',
        'brFirmware_scripts',
    ]
    for name in targets:
        if name in symbols:
            fw_syms[name] = symbols[name]
            print(f"  Found {name}: addr=0x{symbols[name]['addr']:x}, "
                  f"size={symbols[name]['size']}")
        else:
            print(f"  WARNING: {name} not found in symbol table")

    return fw_syms


def extract_data(so_path, sections, sym_addr, size, max_size=1024*1024):
    """Extract bytes from the binary at a given virtual address."""
    # Find which section contains this address
    for sec_name, sec in sections.items():
        if sec['vma'] <= sym_addr < sec['vma'] + sec['size']:
            offset = sec['file_offset'] + (sym_addr - sec['vma'])
            if size == 0:
                # Try to detect size - read up to next symbol or section end
                size = min(sec['size'] - (sym_addr - sec['vma']), max_size)

            with open(so_path, 'rb') as f:
                f.seek(offset)
                data = f.read(size)
            print(f"  Extracted {len(data)} bytes from section {sec_name} "
                  f"at file offset 0x{offset:x}")
            return data

    print(f"  ERROR: Address 0x{sym_addr:x} not found in any section")
    return None


def generate_header(fw_data, output_path):
    """Generate C header file with firmware data."""
    with open(output_path, 'w') as f:
        f.write("/* Auto-generated by extract_firmware.py */\n")
        f.write("/* IT9300 USB Bridge Firmware Data */\n")
        f.write("#ifndef BR_FIRMWARE_H\n")
        f.write("#define BR_FIRMWARE_H\n\n")
        f.write("#include <stdint.h>\n\n")

        for name, data in fw_data.items():
            if data is None:
                f.write(f"/* {name}: NOT FOUND */\n\n")
                continue

            c_name = name
            f.write(f"static const uint8_t {c_name}[] = {{\n")

            for i in range(0, len(data), 16):
                chunk = data[i:i+16]
                hex_vals = ', '.join(f'0x{b:02x}' for b in chunk)
                f.write(f"    {hex_vals},\n")

            f.write(f"}};\n")
            f.write(f"static const uint32_t {c_name}_len = {len(data)};\n\n")

        f.write("#endif /* BR_FIRMWARE_H */\n")

    print(f"\nGenerated: {output_path}")
    print(f"Total firmware size: {sum(len(d) for d in fw_data.values() if d)} bytes")


def main():
    if len(sys.argv) < 2:
        print("Usage: extract_firmware.py <liba3_phy_sony.so> [output_dir]")
        sys.exit(1)

    so_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else '.'

    if not os.path.exists(so_path):
        print(f"Error: {so_path} not found")
        sys.exit(1)

    print(f"=== IT9300 Firmware Extractor ===\n")
    print(f"Input: {so_path}")
    print(f"Output: {output_dir}\n")

    # Step 1: Get symbols
    print("[1] Reading symbol table...")
    symbols = run_nm(so_path)
    print(f"  Found {len(symbols)} symbols total\n")

    # Step 2: Find firmware symbols
    print("[2] Looking for firmware symbols...")
    fw_syms = find_firmware_symbols(symbols)

    if not fw_syms:
        print("\nNo firmware symbols found!")
        print("The firmware data might use different symbol names.")
        print("\nSearching for alternative firmware-related symbols...")
        for name, info in sorted(symbols.items()):
            if 'firmware' in name.lower() or 'Firmware' in name:
                print(f"  {name}: addr=0x{info['addr']:x}, size={info['size']}")
        sys.exit(1)

    # Step 3: Get section layout
    print("\n[3] Reading section layout...")
    sections = run_objdump_sections(so_path)
    for sec_name in ['.rodata', '.data', '.bss']:
        if sec_name in sections:
            sec = sections[sec_name]
            print(f"  {sec_name}: vma=0x{sec['vma']:x}, "
                  f"size={sec['size']}, file_offset=0x{sec['file_offset']:x}")

    # Step 4: Extract firmware data
    print("\n[4] Extracting firmware data...")
    fw_data = {}
    for name, info in fw_syms.items():
        print(f"\n  Extracting {name}...")
        data = extract_data(so_path, sections, info['addr'], info['size'])
        fw_data[name] = data

    # Step 5: Generate header
    print("\n[5] Generating C header...")
    output_path = os.path.join(output_dir, 'br_firmware.h')
    generate_header(fw_data, output_path)

    print("\nDone! Include this header in your IT9300 bridge driver.")
    print(f"Copy to: src/bridge/br_firmware.h")


if __name__ == '__main__':
    main()
