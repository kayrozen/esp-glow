#!/usr/bin/env python3
# fix_makefile_f4.py — extend clean target for MIDI/OSC/WEB objects + re-fix tabs.
import pathlib, re, subprocess

MF = pathlib.Path("/home/z/my-project/work/esp-glow/Makefile")
t = MF.read_text()

new_clean = (
    "clean:\n"
    "\trm -f $(AIM_OBJECTS) $(AIM_TARGET) $(FP_OBJECTS) $(FP_TARGET) $(SHOW_OBJECTS) $(SHOW_TARGET) \\\n"
    "              $(EFFECTS_OBJECTS) $(EFFECTS_TARGET) $(SHOW_CONTROL_OBJECTS) $(SHOW_CONTROL_TARGET) \\\n"
    "              $(PIXEL_MATRIX_OBJECTS) $(PIXEL_MATRIX_TARGET) $(PROVISION_OBJECTS) $(PROVISION_TARGET) \\\n"
    "              $(PACING_OBJECTS) $(PACING_TARGET) $(APPLY_OBJECTS) $(APPLY_TARGET) \\\n"
    "              $(MIDI_OBJECTS) $(MIDI_TARGET) $(OSC_OBJECTS) $(OSC_TARGET) $(WEB_OBJECTS) $(WEB_TARGET)\n"
)
pat = re.compile(r"clean:\n[ \t]+rm -f \$\(AIM_OBJECTS\).*?\$\(APPLY_TARGET\)\n", re.S)
if not pat.search(t):
    raise SystemExit("clean target not found")
t = pat.sub(new_clean, t)
MF.write_text(t)
print("clean target updated for F4 (MIDI/OSC/WEB).")
subprocess.run(["python3", "/home/z/my-project/work/esp-glow/scripts/fix_makefile_tabs.py"], check=True)
