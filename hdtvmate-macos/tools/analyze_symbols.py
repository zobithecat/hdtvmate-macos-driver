#!/usr/bin/env python3
"""
analyze_symbols.py - Analyze symbols in liba3_phy_sony.so

Generates reports of:
- All exported functions organized by subsystem
- Source file paths from debug info
- Function signatures and cross-references
- Decompilation targets (key functions for Ghidra analysis)

Usage:
    python3 analyze_symbols.py <path_to_liba3_phy_sony.so>
"""

import subprocess
import sys
import os
from collections import defaultdict


def get_symbols(so_path):
    """Get all dynamic symbols."""
    result = subprocess.run(
        ['nm', '-D', so_path],
        capture_output=True, text=True
    )
    symbols = []
    for line in result.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3:
            addr = int(parts[0], 16)
            sym_type = parts[1]
            name = parts[2]
            symbols.append({'addr': addr, 'type': sym_type, 'name': name})
    return symbols


def get_debug_paths(so_path):
    """Extract source file paths from debug info."""
    result = subprocess.run(
        ['strings', so_path],
        capture_output=True, text=True
    )
    paths = set()
    for line in result.stdout.splitlines():
        if '/' in line and ('.c' in line or '.cpp' in line or '.h' in line):
            # Filter for source paths
            if 'a3devprj' in line or 'sony' in line.lower() or 'ite' in line.lower():
                paths.add(line.strip())
    return sorted(paths)


def categorize_symbols(symbols):
    """Categorize symbols by subsystem."""
    categories = defaultdict(list)

    for sym in symbols:
        if sym['type'] != 'T':  # Only text (function) symbols
            continue
        name = sym['name']

        if name.startswith('DRV_CXD6801'):
            categories['CXD6801 Driver (High-level)'].append(sym)
        elif name.startswith('DRV_CXD2880'):
            categories['CXD2880 Driver'].append(sym)
        elif name.startswith('DRV_CXD285'):
            categories['CXD285X Driver'].append(sym)
        elif name.startswith('sony_cxd6801_demod_atsc3_monitor'):
            categories['CXD6801 ATSC3 Monitor'].append(sym)
        elif name.startswith('sony_cxd6801_demod_atsc3'):
            categories['CXD6801 ATSC3 Demod'].append(sym)
        elif name.startswith('sony_cxd6801_demod_atsc'):
            categories['CXD6801 ATSC1 Demod'].append(sym)
        elif name.startswith('sony_cxd6801_integ'):
            categories['CXD6801 Integration'].append(sym)
        elif name.startswith('sony_cxd6801_demod'):
            categories['CXD6801 Demod Core'].append(sym)
        elif name.startswith('sony_cxd6801_ascot3'):
            categories['ASCOT3 Tuner'].append(sym)
        elif name.startswith('sony_cxd6801'):
            categories['CXD6801 Misc'].append(sym)
        elif name.startswith('IT9300'):
            categories['IT9300 Bridge API'].append(sym)
        elif name.startswith('BrCmd') or name.startswith('Cmd_'):
            categories['Bridge Commands'].append(sym)
        elif name.startswith('BrUser'):
            categories['Bridge User (Platform)'].append(sym)
        elif name.startswith('DL_'):
            categories['Driver Layer (DL)'].append(sym)
        elif name.startswith('DRV_'):
            categories['Other Drivers'].append(sym)
        elif name.startswith('Java_'):
            categories['JNI Functions'].append(sym)
        elif 'SonyPHYAndroid' in name:
            categories['SonyPHYAndroid Class'].append(sym)
        elif 'IAtsc3NdkPHYClient' in name:
            categories['PHY Client Interface'].append(sym)
        elif name.startswith('drvi2c'):
            categories['I2C Drivers'].append(sym)
        elif name.startswith('libusb'):
            categories['libusb Functions'].append(sym)
        elif name.startswith('atsc3_') or name.startswith('atsc1_'):
            categories['ATSC Protocol'].append(sym)

    return categories


def print_report(categories, debug_paths):
    """Print analysis report."""
    print("=" * 70)
    print("liba3_phy_sony.so Symbol Analysis Report")
    print("=" * 70)

    total = sum(len(syms) for syms in categories.values())
    print(f"\nTotal categorized functions: {total}")
    print(f"Categories: {len(categories)}\n")

    for cat_name in sorted(categories.keys()):
        syms = categories[cat_name]
        print(f"\n--- {cat_name} ({len(syms)} functions) ---")
        for sym in sorted(syms, key=lambda s: s['addr']):
            print(f"  0x{sym['addr']:08x}  {sym['name']}")

    # Key functions for Ghidra
    print("\n\n" + "=" * 70)
    print("Priority Ghidra Decompilation Targets")
    print("=" * 70)

    priority_funcs = [
        ('SonyPHYAndroid::open', 'USB open + endpoint discovery'),
        ('sony_cxd6801_demod_Initialize', 'Demod init register sequence'),
        ('sony_cxd6801_demod_atsc3_Tune', 'ATSC3 tune registers'),
        ('sony_cxd6801_ascot3_Tune', 'Tuner PLL calculation'),
        ('sony_cxd6801_ascot3_Initialize', 'Tuner init sequence'),
        ('DRV_CXD6801_initialize', 'Full CXD6801 init flow'),
        ('DRV_CXD6801_acquireChannel', 'Channel acquisition'),
        ('BrUser_busTx', 'USB TX protocol'),
        ('BrUser_busRx', 'USB RX protocol'),
        ('BrCmd_loadFirmware', 'Firmware upload protocol'),
        ('IT9300_initialize', 'Bridge init sequence'),
    ]

    for func_name, desc in priority_funcs:
        found = False
        for cat_syms in categories.values():
            for sym in cat_syms:
                if func_name in sym['name']:
                    print(f"  0x{sym['addr']:08x}  {sym['name']}  -- {desc}")
                    found = True
        if not found:
            print(f"  NOT FOUND: {func_name}  -- {desc}")

    # Source paths
    if debug_paths:
        print("\n\n" + "=" * 70)
        print(f"Source File Paths ({len(debug_paths)} files)")
        print("=" * 70)
        for path in debug_paths:
            print(f"  {path}")


def main():
    if len(sys.argv) < 2:
        print("Usage: analyze_symbols.py <liba3_phy_sony.so>")
        sys.exit(1)

    so_path = sys.argv[1]
    if not os.path.exists(so_path):
        print(f"Error: {so_path} not found")
        sys.exit(1)

    print(f"Analyzing: {so_path}\n")

    symbols = get_symbols(so_path)
    debug_paths = get_debug_paths(so_path)
    categories = categorize_symbols(symbols)

    print_report(categories, debug_paths)


if __name__ == '__main__':
    main()
