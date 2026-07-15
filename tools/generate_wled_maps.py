#!/usr/bin/env python3
"""
generate_wled_maps.py

Parses WLED source files and generates C++ effect/palette maps.
Usage:
    python tools/generate_wled_maps.py \
        --fx-cpp deps/wled/wled00/FX.cpp \
        --palettes-h deps/wled/wled00/palettes.h \
        --out src/wled_effect_map.h \
        --wled-version 0.14.0
"""

import re
import argparse
from pathlib import Path


def slugify(name: str) -> str:
    """Convert WLED effect name to kebab-case Fennel keyword."""
    # "Fire 2012" -> "fire-2012", "Sparkle+" -> "sparkle+", "C9 2" -> "c9-2"
    name = name.strip().lower()
    name = re.sub(r'[^\w+]+', '-', name)
    name = name.strip('-')
    return name


def parse_fx_cpp(path: Path) -> list[tuple[int, str, str]]:
    """Extract JSON_mode_names[] from FX.cpp."""
    text = path.read_text()
    # Find the array: const char* JSON_mode_names[] = { ... };
    match = re.search(r'JSON_mode_names\[\]\s*=\s*\{(.*?)\};', text, re.DOTALL)
    if not match:
        raise ValueError("Could not find JSON_mode_names[] in FX.cpp")

    items = re.findall(r'"([^"]+)"', match.group(1))
    effects = []
    for i, name in enumerate(items):
        if name:  # Skip empty entries
            effects.append((i, slugify(name), name))
    return effects


def parse_palettes_h(path: Path) -> list[tuple[int, str, str]]:
    """Extract palette names from palettes.h."""
    text = path.read_text()
    palettes = []
    
    # Look for palette name definitions in various formats
    # WLED stores palette names in different ways depending on version
    
    # Method 1: Look for gradientPalette_names[] array
    match = re.search(r'gradientPalette_names\[\]\s*=\s*\{(.*?)\};', text, re.DOTALL)
    if match:
        items = re.findall(r'"([^"]+)"', match.group(1))
        for i, name in enumerate(items):
            if name:
                palettes.append((i, slugify(name), name))
        return palettes
    
    # Method 2: Look for comments with palette names or PROGMEM strings
    # This is a fallback for different WLED versions
    lines = text.split('\n')
    palette_id = 0
    for line in lines:
        # Look for patterns like: "Default Palette\0" or similar
        match = re.search(r'"([^"]+)"', line)
        if match and 'palette' in line.lower():
            name = match.group(1)
            if name and not name.startswith('_'):
                palettes.append((palette_id, slugify(name), name))
                palette_id += 1
    
    return palettes


def generate_header(effects: list[tuple[int, str, str]], 
                    palettes: list[tuple[int, str, str]], 
                    version: str) -> str:
    """Generate the C++ header file content."""
    lines = [
        '#pragma once',
        '#include <unordered_map>',
        '#include <string>',
        '#include <cstdint>',
        '',
        f'// AUTO-GENERATED from WLED v{version}',
        '// DO NOT EDIT MANUALLY — run tools/generate_wled_maps.py to regenerate',
        '//',
        '// Effect IDs are STABLE within a WLED minor version but may shift across',
        '// major versions. This map is version-locked.',
        '',
        'namespace wled {',
        '',
        f'inline constexpr uint8_t EFFECT_COUNT = {len(effects)};',
        f'inline constexpr uint8_t PALETTE_COUNT = {len(palettes)};',
        '',
        '// Effect name -> ID mapping (kebab-case names)',
        'const std::unordered_map<std::string, uint8_t> EFFECT_MAP = {',
    ]
    
    for id_, slug, orig in effects:
        lines.append(f'    {{"{slug}", {id_}}}, // {orig}')
    
    lines.append('};')
    lines.append('')
    lines.append('// Palette name -> ID mapping (kebab-case names)')
    lines.append('const std::unordered_map<std::string, uint8_t> PALETTE_MAP = {')
    
    for id_, slug, orig in palettes:
        lines.append(f'    {{"{slug}", {id_}}}, // {orig}')
    
    lines.append('};')
    lines.append('')
    lines.append('// Safe lookup with fallback and logging')
    lines.append('inline uint8_t effectId(const std::string& name) {')
    lines.append('    auto it = EFFECT_MAP.find(name);')
    lines.append('    if (it != EFFECT_MAP.end()) return it->second;')
    lines.append('    // Fallback: log warning and return solid (0)')
    lines.append('    // ESP_LOGW("WLED", "Unknown effect \'%s\', falling back to solid", name.c_str());')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('inline uint8_t paletteId(const std::string& name) {')
    lines.append('    auto it = PALETTE_MAP.find(name);')
    lines.append('    if (it != PALETTE_MAP.end()) return it->second;')
    lines.append('    // Fallback: log warning and return default (0)')
    lines.append('    // ESP_LOGW("WLED", "Unknown palette \'%s\', falling back to default", name.c_str());')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('} // namespace wled')
    
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Generate WLED effect/palette maps from WLED source files'
    )
    parser.add_argument('--fx-cpp', required=True, type=Path,
                        help='Path to WLED FX.cpp file')
    parser.add_argument('--palettes-h', required=True, type=Path,
                        help='Path to WLED palettes.h file')
    parser.add_argument('--out', required=True, type=Path,
                        help='Output path for generated header')
    parser.add_argument('--wled-version', required=True,
                        help='WLED version string (e.g., 0.14.0)')
    
    args = parser.parse_args()
    
    if not args.fx_cpp.exists():
        print(f"Error: FX.cpp not found at {args.fx_cpp}")
        return 1
    
    if not args.palettes_h.exists():
        print(f"Error: palettes.h not found at {args.palettes_h}")
        return 1
    
    try:
        effects = parse_fx_cpp(args.fx_cpp)
        palettes = parse_palettes_h(args.palettes_h)
        
        header = generate_header(effects, palettes, args.wled_version)
        args.out.write_text(header)
        
        print(f"Generated {args.out} with {len(effects)} effects and {len(palettes)} palettes")
        return 0
        
    except Exception as e:
        print(f"Error generating maps: {e}")
        return 1


if __name__ == '__main__':
    exit(main())
