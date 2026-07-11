#!/usr/bin/env python3
# fix_makefile_f3.py — extend the clean target for the new APPLY objects and
# re-run the tab fixer. (Edit tool mangles tabs; this is the reliable path.)
import pathlib, re

MF = pathlib.Path("/home/z/my-project/work/esp-glow/Makefile")
t = MF.read_text()

# Replace the clean target's rm list to include PACING and APPLY objects.
old_rm = ("clean:\n"
          "        rm -f $(AIM_OBJECTS) $(AIM_TARGET) $(FP_OBJECTS) $(FP_TARGET) $(SHOW_OBJECTS) $(SHOW_TARGET) \\\n"
          "              $(EFFECTS_OBJECTS) $(EFFECTS_TARGET) $(SHOW_CONTROL_OBJECTS) $(SHOW_CONTROL_TARGET) \\\n"
          "              $(PIXEL_MATRIX_OBJECTS) $(PIXEL_MATRIX_TARGET) $(PROVISION_OBJECTS) $(PROVISION_TARGET)\n")
new_rm = ("clean:\n"
          "\trm -f $(AIM_OBJECTS) $(AIM_TARGET) $(FP_OBJECTS) $(FP_TARGET) $(SHOW_OBJECTS) $(SHOW_TARGET) \\\n"
          "              $(EFFECTS_OBJECTS) $(EFFECTS_TARGET) $(SHOW_CONTROL_OBJECTS) $(SHOW_CONTROL_TARGET) \\\n"
          "              $(PIXEL_MATRIX_OBJECTS) $(PIXEL_MATRIX_TARGET) $(PROVISION_OBJECTS) $(PROVISION_TARGET) \\\n"
          "              $(PACING_OBJECTS) $(PACING_TARGET) $(APPLY_OBJECTS) $(APPLY_TARGET)\n")

# Tolerate either spaces or a tab on the rm line by matching loosely.
pat = re.compile(r"clean:\n[ \t]+rm -f \$\(AIM_OBJECTS\).*?\$\(PROVISION_TARGET\)\n", re.S)
if not pat.search(t):
    raise SystemExit("could not find clean target to replace")
t = pat.sub(new_rm, t)
MF.write_text(t)
print("clean target updated with APPLY objects + tab.")

# Now re-run the generic tab fixer for any other recipe lines.
import subprocess
subprocess.run(["python3", "/home/z/my-project/work/esp-glow/scripts/fix_makefile_tabs.py"], check=True)
