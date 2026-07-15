#pragma once

#include "fixture_profile.h"
#include "aim.h"
#include "pixel_matrix.h"
#include "mdef.h"
#include "show.h"
#include "wled_target.h"
#include <vector>
#include <cstdint>

// SHW1 bundle format: little-endian, portable binary encoding
//
// Version 1 header:
//   magic         4 bytes "SHW1"
//   version       u8 = 1
//   universeCount u8
//   profileCount  u16
//   fixtureCount  u16
//   matrixCount   u16
//
// Version 2 header inserts one extra field after matrixCount (mirrors
// PFX1 v2 inserting rangeCount into its header -- see FORMAT.md): everything
// through the matrix table is otherwise byte-identical to v1.
//   ...
//   matrixCount   u16
//   mdefCount     u16   (v2+)
//
// Version 3 header inserts one more field after mdefCount, same additive
// convention -- everything through the controller table is byte-identical
// to v2. mdefCount is always present at v3+ (0 if the show has no
// CONTROLLER) so the header stays self-describing without a per-version
// three-way branch past this point.
//   ...
//   mdefCount     u16   (v2+)
//   wledCount     u16   (v3+)
//
// Version 4 (Wave 3): same header as v3 (carries mdefCount + wledCount),
// but the universe table entries grow from 1 byte to 7 -- see below.
// Emitted only when the .show actually used the new `UNIVERSE ... ARTNET
// <ip> [<wireUniverse>]` grammar (provision.cpp's compileShow); a .show
// with no explicit Art-Net routing still compiles to v1/v2/v3 bytes,
// unchanged.
//
// Universe table (universeCount entries):
//   v1/v2/v3 -- 1 byte each:
//     transport     u8   (0=Dmx, 1=ArtNet, 2=Sacn, 3=Unused)
//   v4 -- 7 bytes each:
//     transport     u8   (as above)
//     destIp        u32  (packed host-byte-order IPv4; 0 = no explicit route --
//                          see ArtNetDest in show.h for the fallback/broadcast
//                          precedence this feeds into)
//     wireUniverse  u16  (Art-Net's 15-bit Port-Address, 0..32767; already
//                          resolved by the compiler -- defaults to the
//                          internal 0-indexed universe number when the .show
//                          didn't specify one explicitly)
// Profile table (profileCount entries):
//   blobLen       u16
//   blob          blobLen bytes   (a PFX1 profile)
// Fixture table (fixtureCount entries, fixed 46-byte records):
//   profileIndex  u16
//   universe      u8
//   base          u16
//   isHead        u8
//   posX,posY,posZ            f32 f32 f32
//   yaw,pitch,roll            f32 f32 f32   (degrees)
//   panRangeDeg,tiltRangeDeg  f32 f32
//   panCenterNorm,tiltCenterNorm f32 f32
//   invertPan,invertTilt      u8 u8
//   (head fields are zeroed for non-head fixtures)
// Matrix table (matrixCount entries):
//   width,height  u16 u16
//   serpentine    u8
//   vertical      u8
//   order         u8            (ColorOrder value)
//   startUniverse u8
//   startChannel  u16
// Controller (mdef) table (v2+ only, mdefCount entries):
//   blobLen       u16
//   blob          blobLen bytes   (an MDF1 controller definition, mdef.h)
// WLED target table (v3+ only, wledCount entries):
//   nameLen       u8
//   name          nameLen bytes  (UTF-8, not NUL-terminated)
//   ipLen         u8
//   ip            ipLen bytes    (UTF-8 IPv4 dotted-quad or 255.255.255.255)
//   port          u16
//   syncGroup     u8

enum class UniverseTransport : uint8_t {
  Dmx = 0,
  ArtNet = 1,
  Sacn = 2,
  Unused = 3
};

struct PatchEntry {
  FixtureProfile profile;
  uint8_t universe;
  uint16_t base;
  bool isHead;
  MovingHeadConfig head;  // built via mat3FromEuler(yaw,pitch,roll) when isHead
};

struct LoadedShow {
  uint8_t universeCount = 0;
  UniverseTransport transport[8] = {};
  // Wave 3: per-universe Art-Net destination, indexed the same as transport[].
  // Always populated, regardless of bundle version -- a v1/v2/v3 bundle (no
  // per-universe routing table on the wire) gets today's implicit default
  // here: {ip=0 (fallback/broadcast), wireUniverse=the universe's own
  // internal index}, exactly matching pre-Wave-3 behavior. Only meaningful
  // for universes whose transport[i] == ArtNet.
  ArtNetDest artnetDest[8] = {};
  std::vector<PatchEntry> fixtures;
  std::vector<MatrixMap> matrices;
  std::vector<MidiControllerProfile> controllers;  // v2+ only; usually 0 or 1 (see B1)
  std::vector<WledTarget> wledTargets;              // v3+ only
};

// Load a SHW1 bundle from a byte buffer.
// Strict: returns false on bad magic/version, size overrun, profileIndex out of
// range, or any PFX1 blob that fails parseProfile, or (v2+) any MDF1 blob
// that fails parseMidiController. Never reads out of bounds.
bool loadShow(const uint8_t* data, size_t len, LoadedShow& out);
