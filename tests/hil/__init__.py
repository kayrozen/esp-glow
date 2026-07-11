"""
ESP-Glow Hardware-in-the-Loop (HIL) Test Suite

This package contains automated tests for the esp-glow firmware
running on ESP32-S3, validating boot, DMX/Art-Net output, show loading,
live control inputs, stress testing, and OTA/fault recovery.

Test layers:
- L0: Boot/POST
- L1: DMX output
- L2: Art-Net output
- L3: Show load
- L4: Inputs (Web/OSC/MIDI)
- L5: Soak/concurrency (stress test)
- L6: OTA and robustness

See README.md for detailed documentation.
"""

__version__ = "1.0.0"
__author__ = "ESP-Glow Project"
