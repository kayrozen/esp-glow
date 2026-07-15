#!/usr/bin/env python3
"""
generate_wled_maps.py — regenerate wled_effect_map.h from a checked-out
WLED source tree.

WLED's FX.cpp declares its effect/palette name tables as PROGMEM raw string
literals holding a JSON array, e.g.:

    const char JSON_mode_names[] PROGMEM = R"=====(["Solid","Blink@!,;!,;!;;01"...])=====";
    const char JSON_palette_names[] PROGMEM = R"=====(["Default","Random Cycle",...])=====";

Each entry may carry "@<ui hints>" metadata after the display name (slider
labels, color slot counts, etc., consumed by WLED's own UI) -- everything
from '@' onward is stripped before slugifying.

Usage:
    python3 tools/generate_wled_maps.py \\
        --fx-cpp deps/wled/wled00/FX.cpp \\
        --out wled_effect_map.h \\
        --wled-version 0.14.0

NOTE: this is a best-effort regenerator, not a build-time dependency (no
target in Makefile/CMakeLists.txt runs it) -- WLED's exact source format has
drifted across versions and isn't guaranteed stable. ALWAYS diff its output
against the committed wled_effect_map.h and re-run `make test_wled` before
adopting a regenerated file; a mismatch there is a review, not a rubber
stamp, since every ID in this file is also baked into every .show/Fennel
script that names an effect/palette by string (see README_WLED.md).
"""

import argparse
import re
import sys
from pathlib import Path


def slugify(name: str) -> str:
    """WLED display name -> kebab-case Fennel keyword.

    "Fire 2012" -> "fire-2012", "Sparkle+" -> "sparkle+", "C9 2" -> "c9-2".
    '+' is kept (WLED itself uses it, e.g. "Sparkle+") -- everything else
    non-alphanumeric becomes a single '-', collapsed and trimmed.
    """
    name = name.strip().lower()
    name = re.sub(r"[^\w+]+", "-", name)
    name = name.strip("-")
    return name


def extract_json_string_array(text: str, var_name: str) -> list[str]:
    """Extract the quoted names from `const char <var_name>[] PROGMEM =
    R"=====([...])=====";` (WLED's actual FX.cpp declaration shape).
    Falls back to a plain `<var_name>[] = { "a", "b" };` C-array declaration
    for older/alternate formats. Raises ValueError if neither matches.
    """
    raw_string = re.search(
        rf'{re.escape(var_name)}\s*\[\]\s*PROGMEM\s*=\s*R"([^(]*)\((.*?)\)\1";',
        text,
        re.DOTALL,
    )
    if raw_string:
        body = raw_string.group(2)
    else:
        c_array = re.search(rf"{re.escape(var_name)}\s*\[\]\s*=\s*\{{(.*?)\}};", text, re.DOTALL)
        if not c_array:
            raise ValueError(f"could not find {var_name} in the given source")
        body = c_array.group(1)

    names = []
    for raw in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        display = raw.split("@", 1)[0]  # strip WLED's "@ui hints" suffix
        if display:
            names.append(display)
    return names


def build_map(names: list[str]) -> list[tuple[int, str, str]]:
    seen: dict[str, int] = {}
    out = []
    for i, display in enumerate(names):
        slug = slugify(display)
        if not slug:
            continue
        if slug in seen:
            # WLED occasionally reuses a display name; disambiguate instead
            # of silently colliding two IDs onto one Fennel keyword.
            slug = f"{slug}-{i}"
        seen[slug] = i
        out.append((i, slug, display))
    return out


def generate_header(effects: list[tuple[int, str, str]], palettes: list[tuple[int, str, str]],
                    version: str) -> str:
    lines = [
        "#pragma once",
        "#include <unordered_map>",
        "#include <string>",
        "#include <cstdint>",
        "",
        f"// AUTO-GENERATED from WLED v{version}'s FX.cpp -- run",
        "// tools/generate_wled_maps.py to regenerate. DO NOT EDIT MANUALLY.",
        "// See README_WLED.md for the review step before adopting a regenerated file.",
        "",
        "namespace wled {",
        "",
        f"inline constexpr uint16_t EFFECT_COUNT = {len(effects)};",
        f"inline constexpr uint16_t PALETTE_COUNT = {len(palettes)};",
        "",
        "inline const std::unordered_map<std::string, uint8_t> EFFECT_MAP = {",
    ]
    for id_, slug, display in effects:
        lines.append(f'    {{"{slug}", {id_}}},  // {display}')
    lines.append("};")
    lines.append("")
    lines.append("inline const std::unordered_map<std::string, uint8_t> PALETTE_MAP = {")
    for id_, slug, display in palettes:
        lines.append(f'    {{"{slug}", {id_}}},  // {display}')
    lines.append("};")
    lines.append("")
    lines.append("inline bool effectIdFromName(const std::string& name, uint8_t& out) {")
    lines.append("  auto it = EFFECT_MAP.find(name);")
    lines.append("  if (it == EFFECT_MAP.end()) return false;")
    lines.append("  out = it->second;")
    lines.append("  return true;")
    lines.append("}")
    lines.append("")
    lines.append("inline bool paletteIdFromName(const std::string& name, uint8_t& out) {")
    lines.append("  auto it = PALETTE_MAP.find(name);")
    lines.append("  if (it == PALETTE_MAP.end()) return false;")
    lines.append("  out = it->second;")
    lines.append("  return true;")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace wled")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fx-cpp", required=True, type=Path,
                        help="path to WLED's wled00/FX.cpp (has both name tables)")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--wled-version", required=True)
    args = parser.parse_args()

    text = args.fx_cpp.read_text()

    try:
        effect_names = extract_json_string_array(text, "JSON_mode_names")
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    try:
        palette_names = extract_json_string_array(text, "JSON_palette_names")
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    effects = build_map(effect_names)
    palettes = build_map(palette_names)

    header = generate_header(effects, palettes, args.wled_version)
    args.out.write_text(header)
    print(f"Generated {args.out} with {len(effects)} effects and {len(palettes)} palettes")
    print("Review the diff and re-run `make test_wled` before committing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
