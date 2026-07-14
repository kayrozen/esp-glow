#!/usr/bin/env bash
# build_sample_bundle.sh — compile the provision tool and build the demo SHW1
# bundle that ships in LittleFS so F3 is testable on hardware immediately.
#
# Output: firmware/main/data/show.shw1
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# Compile the provision compiler (provision_main + provision + profile_encoder
# + their deps). The Makefile does not build this binary, so we do it here.
g++ -std=c++17 -Wall -Wextra -Werror -O2 \
    provision_main.cpp provision.cpp profile_encoder.cpp controller_encoder.cpp \
    fixture_profile.cpp mdef.cpp pixel_matrix.cpp color.cpp vec_math.cpp aim.cpp \
    show_bundle.cpp \
    -o /tmp/provision -lm

mkdir -p firmware/main/data
/tmp/provision samples/demo.show firmware/main/data/show.shw1

ls -l firmware/main/data/show.shw1
echo "Bundle built. It will be flashed to the LittleFS partition via"
echo "littlefs_create_partition_image() in firmware/main/CMakeLists.txt."
