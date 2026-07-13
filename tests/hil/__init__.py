"""
ESP-Glow Hardware-in-the-Loop (HIL) Test Suite

This package contains automated tests for the esp-glow firmware running on
ESP32-S3: boot/POST, DMX/Art-Net output, show loading from the raw "show"
partition, live control inputs (Web/OSC/MIDI), the Fennel live-coding REPL
over WebSocket, scripting safety guarantees (fx_error, infinite loop, OOM),
a 10-minute soak, and F5 robustness (OTA/AP-pull/WDT -- mostly skipped
until F5 lands).

Test layers:
- L0: Boot/POST
- L1: DMX output (?dmx0 serial query)
- L2: Art-Net output
- L3: Show load (raw "show" partition)
- L4: Inputs (Web/OSC fully automated; MIDI semi-automated)
- L5: Fennel REPL / live-coding over WS
- L6: Safety guarantees
- L7: Soak (10 min, run last)
- L8: F5 robustness (when F5 lands)

See README.md for detailed documentation.
"""

__version__ = "2.0.0"
__author__ = "ESP-Glow Project"
