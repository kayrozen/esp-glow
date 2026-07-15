// test_mdef.cpp — MDF1 controller-definition binary format tests. The MIDI
// twin of test_fixture_profile.cpp: parse/round-trip the binary format,
// prove the security-boundary validation, and the two behaviors called out
// explicitly in the design (A6): encoder mode is parsed, and an unknown
// colour name is a no-op, not an error.

#include "mdef.h"
#include "controller_encoder.h"

#include <cstdio>
#include <cstring>

static int g_failCount = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++;                                          \
    }                                                          \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

namespace {

// A minimal APC40-mkII-shaped controller: 8 pads (53..60), one named fader,
// one relative encoder, and an LED range with a small colour palette.
ControllerBuilder makeController() {
  ControllerBuilder b;
  b.name = "Test Controller";
  b.midiChannel = 1;
  b.pads.push_back({53, 60});
  b.faders.push_back({7, 7, "master"});
  b.faders.push_back({48, 55, ""});
  b.encoders.push_back({16, 16, EncoderMode::Relative2C});

  ControllerLedSpec led;
  led.msgType = LedMsgType::Note;
  led.addrFrom = 53;
  led.addrTo = 60;
  led.semantic = LedSemantic::Velocity;
  led.colors.push_back({"off", 0});
  led.colors.push_back({"green", 1});
  led.colors.push_back({"red", 3});
  b.leds.push_back(led);

  return b;
}

}  // namespace

void test_roundtrip_basic_fields() {
  TEST("Round-trip: pads/faders/encoders/LED ranges/colour palette");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());
  CHECK(!blob.empty());
  CHECK(blob[0] == 'M' && blob[1] == 'D' && blob[2] == 'F' && blob[3] == '1');
  CHECK(blob[4] == 1);  // version

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));

  CHECK(p.midiChannel == 1);
  CHECK(p.padCount == 1);
  CHECK(p.pads[0].noteFrom == 53);
  CHECK(p.pads[0].noteTo == 60);

  CHECK(p.faderCount == 2);
  CHECK(p.faders[0].ccFrom == 7);
  CHECK(p.faders[0].ccTo == 7);
  CHECK(p.faders[0].nameOff != 0xFFFF);
  CHECK(std::strcmp(reinterpret_cast<const char*>(&p.nameBlob[p.faders[0].nameOff]), "master") == 0);
  CHECK(p.faders[1].ccFrom == 48);
  CHECK(p.faders[1].ccTo == 55);
  CHECK(p.faders[1].nameOff == 0xFFFF);  // unnamed

  CHECK(p.encoderCount == 1);
  CHECK(p.encoders[0].ccFrom == 16);
  CHECK(p.encoders[0].mode == EncoderMode::Relative2C);

  CHECK(p.ledCount == 1);
  CHECK(p.leds[0].msgType == LedMsgType::Note);
  CHECK(p.leds[0].addrFrom == 53);
  CHECK(p.leds[0].addrTo == 60);
  CHECK(p.leds[0].semantic == LedSemantic::Velocity);
  CHECK(p.leds[0].colorCount == 3);
  CHECK(p.colorCount == 3);
}

void test_led_range_lookup_by_note() {
  TEST("findLedRangeForNote finds the declared range; misses elsewhere");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));

  const MdefLedRange* r = findLedRangeForNote(p, 55);
  CHECK(r != nullptr);
  CHECK(r->addrFrom == 53 && r->addrTo == 60);

  CHECK(findLedRangeForNote(p, 61) == nullptr);   // just past the range
  CHECK(findLedRangeForNote(p, 52) == nullptr);   // just before
  CHECK(findLedRangeForCc(p, 55) == nullptr);      // wrong message type
}

void test_color_lookup_by_name() {
  TEST("ledColorValueByName resolves declared colours; unknown name -> false, no-op");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));

  const MdefLedRange* r = findLedRangeForNote(p, 53);
  CHECK(r != nullptr);

  uint8_t value = 255;
  CHECK(ledColorValueByName(p, *r, "off", value));
  CHECK(value == 0);
  CHECK(ledColorValueByName(p, *r, "green", value));
  CHECK(value == 1);
  CHECK(ledColorValueByName(p, *r, "red", value));
  CHECK(value == 3);

  // Unknown colour name: false, and the out-param is left untouched (still
  // holds "red"'s value 3 from the call above) -- exactly the "no-op, not
  // an error" contract A1/A6 call for.
  CHECK(!ledColorValueByName(p, *r, "purple", value));
  CHECK(value == 3);
}

void test_single_pad() {
  TEST("PAD with no explicit range (single pad): noteFrom == noteTo");

  ControllerBuilder b;
  b.name = "Single Pad";
  b.pads.push_back({0, 0});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.padCount == 1);
  CHECK(p.pads[0].noteFrom == 0);
  CHECK(p.pads[0].noteTo == 0);
}

void test_encoder_modes_parsed() {
  TEST("All three encoder modes round-trip distinctly");

  ControllerBuilder b;
  b.name = "Encoders";
  b.encoders.push_back({0, 0, EncoderMode::Absolute});
  b.encoders.push_back({1, 1, EncoderMode::Relative2C});
  b.encoders.push_back({2, 2, EncoderMode::RelativeSignMag});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.encoderCount == 3);
  CHECK(p.encoders[0].mode == EncoderMode::Absolute);
  CHECK(p.encoders[1].mode == EncoderMode::Relative2C);
  CHECK(p.encoders[2].mode == EncoderMode::RelativeSignMag);
}

void test_decode_encoder_delta() {
  TEST("decodeEncoderDelta: two's-complement and sign-magnitude conventions");

  // Absolute: never a delta.
  CHECK(decodeEncoderDelta(EncoderMode::Absolute, 1) == 0);
  CHECK(decodeEncoderDelta(EncoderMode::Absolute, 127) == 0);

  // Two's complement (7-bit): 1..63 positive, 64..127 negative (value-128).
  CHECK(decodeEncoderDelta(EncoderMode::Relative2C, 1) == 1);
  CHECK(decodeEncoderDelta(EncoderMode::Relative2C, 63) == 63);
  CHECK(decodeEncoderDelta(EncoderMode::Relative2C, 127) == -1);
  CHECK(decodeEncoderDelta(EncoderMode::Relative2C, 65) == -63);
  CHECK(decodeEncoderDelta(EncoderMode::Relative2C, 64) == -64);
  CHECK(decodeEncoderDelta(EncoderMode::Relative2C, 0) == 0);

  // Sign-magnitude: bit6 is the sign, low 6 bits the magnitude.
  CHECK(decodeEncoderDelta(EncoderMode::RelativeSignMag, 5) == 5);
  CHECK(decodeEncoderDelta(EncoderMode::RelativeSignMag, 0x45) == -5);  // 0x40 | 5
  CHECK(decodeEncoderDelta(EncoderMode::RelativeSignMag, 0) == 0);
}

void test_bad_magic() {
  TEST("Reject bad magic");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  blob[0] = 'X';

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_bad_version() {
  TEST("Reject unsupported version (4 -- 1, 2, and 3 are all valid)");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  blob[4] = 4;

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_bad_flags() {
  TEST("Reject flags != 0");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  blob[5] = 1;

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_truncated_buffer() {
  TEST("Reject truncated buffer");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  blob.resize(blob.size() - 3);

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_header_too_short() {
  TEST("Reject buffer shorter than the 13-byte header");

  uint8_t buf[10] = {'M', 'D', 'F', '1', 1, 0, 0, 0, 0, 0};
  MidiControllerProfile p;
  CHECK(!parseMidiController(buf, sizeof(buf), p));
}

void test_pad_count_overflow() {
  TEST("Reject padCount > MDEF_MAX_PADS");

  std::vector<uint8_t> blob = {'M', 'D', 'F', '1', 1, 0, 0, 0,
                               static_cast<uint8_t>(MDEF_MAX_PADS + 1), 0, 0, 0, 0};
  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_bad_midi_channel() {
  TEST("Reject midiChannel > 16");

  std::vector<uint8_t> blob = {'M', 'D', 'F', '1', 1, 0, 17, 0, 0, 0, 0, 0, 0};
  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_pad_from_after_to() {
  TEST("Reject a pad record with noteFrom > noteTo");

  ControllerBuilder b;
  b.name = "Bad";
  b.pads.push_back({60, 53});  // from > to
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());  // the encoder itself doesn't validate ordering

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_led_bad_colorOffset() {
  TEST("Reject an LED record whose colorOffset/colorCount overruns the colour table");

  ControllerBuilder b = makeController();
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);

  // Header: magic(4) version(1) flags(1) midiChannel(1) nameLen(1) padCount(1)
  // faderCount(1) encoderCount(1) ledCount(1) colorCount(1) = 13 bytes, then
  // name, then pads(2*1), faders(4*2), encoders(3*1) before the LED table.
  size_t ledTableOffset = 13 + b.name.size() + 2 * 1 + 4 * 2 + 3 * 1;
  // LED record layout: msgType,addrFrom,addrTo,semantic,colorOffset(u16),colorCount
  blob[ledTableOffset + 6] = 0xFF;  // colorCount way beyond the real table (3)

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_encoder_builder_limits() {
  TEST("ControllerBuilder::encode fails cleanly over MDEF_MAX_* limits");

  ControllerBuilder b;
  b.name = "Too Many Pads";
  for (int i = 0; i <= MDEF_MAX_PADS; ++i) {
    b.pads.push_back({static_cast<uint8_t>(i % 128), static_cast<uint8_t>(i % 128)});
  }
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(blob.empty());
  CHECK(!err.empty());
}

// ============================================================================
// Per-range channel significance (MDF1 v2)
// ============================================================================

void test_no_ch_stays_v1_byte_identical() {
  TEST("No CH anywhere -> version 1, byte-identical to before v2 existed");

  ControllerBuilder b = makeController();  // no CH on any pad/fader/LED
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());
  CHECK(blob[4] == 1);  // version byte

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.pads[0].channelFrom == kChannelAgnostic);
  CHECK(p.pads[0].channelTo == kChannelAgnostic);
  CHECK(p.faders[0].channelFrom == kChannelAgnostic);
  CHECK(p.leds[0].channelFrom == kChannelAgnostic);
}

void test_one_ch_range_bumps_to_v2() {
  TEST("A single CH range bumps the whole blob to version 2");

  ControllerBuilder b;
  b.name = "One Channel Range";
  b.pads.push_back({53, 53, 0, 7});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());
  CHECK(blob[4] == 2);

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.pads[0].channelFrom == 0);
  CHECK(p.pads[0].channelTo == 7);
}

void test_ch_0_7_is_8_distinct_logical_pads() {
  TEST("PAD 53 CH 0 7 -> 8 distinct logical pads (via findPadChannelRange + packed id)");

  ControllerBuilder b;
  b.name = "Grid Row";
  b.pads.push_back({53, 53, 0, 7});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));

  const MdefPadRange* r = findPadChannelRange(p, 53);
  CHECK(r != nullptr);
  CHECK(r->channelFrom == 0 && r->channelTo == 7);

  // Every channel 0..7 packs to a distinct id; a channel outside the
  // declared range (8) still resolves to the SAME range (findPadChannelRange
  // only checks the note), but packing with an out-of-declared-range
  // channel is the caller's (LiveControl::effectiveId's) business, not
  // this lookup's -- it only answers "is this note channel-significant".
  std::vector<uint16_t> packedIds;
  for (uint8_t ch = 0; ch <= 7; ++ch) {
    packedIds.push_back(static_cast<uint16_t>((static_cast<uint16_t>(ch) << 8) | 53));
  }
  for (size_t i = 0; i < packedIds.size(); ++i) {
    for (size_t j = 0; j < packedIds.size(); ++j) {
      if (i != j) CHECK(packedIds[i] != packedIds[j]);
    }
  }
}

void test_old_v1_blob_loads_as_agnostic() {
  TEST("An old (hand-built) v1 blob with no channel fields loads as channel-agnostic");

  // Hand-build a minimal v1 blob: header + one pad, no name/faders/etc.
  std::vector<uint8_t> blob = {
    'M', 'D', 'F', '1',
    1,     // version 1
    0,     // flags
    0,     // midiChannel
    0,     // nameLen
    1,     // padCount
    0, 0, 0, 0,  // faderCount, encoderCount, ledCount, colorCount
    53, 60,      // pad record: noteFrom=53, noteTo=60 (v1 shape: 2 bytes, no channel fields)
  };

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.padCount == 1);
  CHECK(p.pads[0].noteFrom == 53 && p.pads[0].noteTo == 60);
  CHECK(p.pads[0].channelFrom == kChannelAgnostic);
  CHECK(p.pads[0].channelTo == kChannelAgnostic);
  CHECK(findPadChannelRange(p, 55) == nullptr);  // agnostic -- not channel-significant
}

void test_channel_range_validation() {
  TEST("v2 channel range validation: mismatched sentinel, out-of-range, from>to all rejected");

  ControllerBuilder base;
  base.name = "X";
  base.pads.push_back({53, 53, 0, 7});
  std::string err;
  std::vector<uint8_t> blob = base.encode(err);
  CHECK(blob[4] == 2);

  // Pad record starts right after the 13-byte header + 1-byte name ("X").
  size_t padOffset = 13 + 1;

  auto tryChannels = [&](uint8_t from, uint8_t to) {
    std::vector<uint8_t> mutated = blob;
    mutated[padOffset + 2] = from;
    mutated[padOffset + 3] = to;
    MidiControllerProfile p;
    return parseMidiController(mutated.data(), mutated.size(), p);
  };

  CHECK(!tryChannels(kChannelAgnostic, 7));   // one sentinel, one real value
  CHECK(!tryChannels(0, kChannelAgnostic));
  CHECK(!tryChannels(5, 2));                  // from > to
  CHECK(!tryChannels(0, 16));                 // 16 is out of range (0..15)
  CHECK(tryChannels(0, 15));                  // full range is valid
  CHECK(tryChannels(kChannelAgnostic, kChannelAgnostic));  // both agnostic is valid
}

void test_resolve_pad_xy_grid() {
  TEST("resolvePadXY: (col,row) -> (note,channel) via declaration-ordered grid rows");

  ControllerBuilder b;
  b.name = "Grid";
  b.pads.push_back({53, 53, 0, 7});  // row 0
  b.pads.push_back({54, 54, 0, 7});  // row 1
  b.pads.push_back({58, 66, kChannelAgnostic, kChannelAgnostic});  // not a grid row (agnostic, multi-note)
  b.pads.push_back({55, 55, 0, 7});  // row 2 (grid rows need not be contiguous with agnostic pads between)
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));

  uint8_t note, channel;
  CHECK(resolvePadXY(p, 0, 0, note, channel));
  CHECK(note == 53 && channel == 0);

  CHECK(resolvePadXY(p, 3, 0, note, channel));
  CHECK(note == 53 && channel == 3);

  CHECK(resolvePadXY(p, 0, 1, note, channel));
  CHECK(note == 54 && channel == 0);

  CHECK(resolvePadXY(p, 0, 2, note, channel));
  CHECK(note == 55 && channel == 0);  // the agnostic multi-note pad is skipped, not counted as a row

  CHECK(!resolvePadXY(p, 0, 3, note, channel));   // no 4th row
  CHECK(!resolvePadXY(p, 8, 0, note, channel));   // col 8 is past channel span 0..7
  CHECK(!resolvePadXY(p, -1, 0, note, channel));  // negative col
  CHECK(!resolvePadXY(p, 0, -1, note, channel));  // negative row
}

// ============================================================================
// P1.1: INIT SYSEX blobs (MDF1 v3)
// ============================================================================

void test_init_blobs_roundtrip() {
  TEST("INIT SYSEX blobs -> version 3, bytes exact, order preserved");

  ControllerBuilder b = makeController();  // no CH ranges -- would be v1 without INIT
  b.initBlobs.push_back({0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x40, 0x00, 0x00, 0x00, 0xF7});
  b.initBlobs.push_back({0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());
  CHECK(blob[4] == 3);  // version byte -- bumped by INIT alone, no CH needed

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.initCount == 2);
  CHECK(p.initBlobs[0].len == 12);
  CHECK(std::memcmp(p.initBlobs[0].data, b.initBlobs[0].data(), 12) == 0);
  CHECK(p.initBlobs[1].len == 6);
  CHECK(std::memcmp(p.initBlobs[1].data, b.initBlobs[1].data(), 6) == 0);

  // The rest of the profile survived the extra header field/table intact.
  CHECK(p.padCount == b.pads.size());
  CHECK(p.leds[0].colorCount == 3);
}

void test_init_blobs_with_channel_ranges_also_v3() {
  TEST("INIT SYSEX + CH ranges together -> still version 3 (v3 implies wide records)");

  ControllerBuilder b;
  b.name = "Both";
  b.pads.push_back({53, 53, 0, 7});
  b.initBlobs.push_back({0xF0, 0x47, 0x00, 0xF7});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());
  CHECK(blob[4] == 3);

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.pads[0].channelFrom == 0 && p.pads[0].channelTo == 7);  // wide records preserved
  CHECK(p.initCount == 1);
  CHECK(p.initBlobs[0].len == 4);
}

void test_no_init_line_sends_nothing() {
  TEST("No INIT line -> initCount 0, every existing .mdef unchanged (v1 and v2)");

  ControllerBuilder v1 = makeController();
  std::string err;
  std::vector<uint8_t> blobV1 = v1.encode(err);
  CHECK(blobV1[4] == 1);
  MidiControllerProfile pV1;
  CHECK(parseMidiController(blobV1.data(), blobV1.size(), pV1));
  CHECK(pV1.initCount == 0);

  ControllerBuilder v2;
  v2.name = "V2 no init";
  v2.pads.push_back({53, 53, 0, 7});
  std::vector<uint8_t> blobV2 = v2.encode(err);
  CHECK(blobV2[4] == 2);
  MidiControllerProfile pV2;
  CHECK(parseMidiController(blobV2.data(), blobV2.size(), pV2));
  CHECK(pV2.initCount == 0);
}

void test_old_v1_v2_blobs_load_with_no_init() {
  TEST("Old MDF1 blob (no init section, hand-built v1/v2) loads, initCount == 0");

  // Hand-built v1 blob (pre-dates INIT entirely) -- exact bytes from
  // test_old_v1_blob_loads_as_agnostic above.
  std::vector<uint8_t> blobV1 = {
    'M', 'D', 'F', '1',
    1, 0, 0, 0, 1, 0, 0, 0, 0,
    53, 60,
  };
  MidiControllerProfile pV1;
  CHECK(parseMidiController(blobV1.data(), blobV1.size(), pV1));
  CHECK(pV1.initCount == 0);

  // Hand-built v2 blob (channel-significant pad, still pre-dates INIT).
  std::vector<uint8_t> blobV2 = {
    'M', 'D', 'F', '1',
    2, 0, 0, 0, 1, 0, 0, 0, 0,
    53, 53, 0, 7,
  };
  MidiControllerProfile pV2;
  CHECK(parseMidiController(blobV2.data(), blobV2.size(), pV2));
  CHECK(pV2.initCount == 0);
  CHECK(pV2.pads[0].channelFrom == 0);
}

void test_init_blob_count_limit() {
  TEST("ControllerBuilder::encode rejects too many INIT SYSEX lines");

  ControllerBuilder b;
  b.name = "Too Many Inits";
  for (int i = 0; i <= MDEF_MAX_INIT_BLOBS; ++i) {
    b.initBlobs.push_back({0xF0, 0xF7});
  }
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(blob.empty());
  CHECK(!err.empty());
}

void test_init_blob_size_limit() {
  TEST("ControllerBuilder::encode rejects an oversized INIT SYSEX blob");

  ControllerBuilder b;
  b.name = "Huge Init";
  std::vector<uint8_t> huge(MDEF_MAX_INIT_BLOB_BYTES + 1, 0x00);
  b.initBlobs.push_back(huge);
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(blob.empty());
  CHECK(!err.empty());
}

void test_init_blob_count_overrun_rejected() {
  TEST("Reject initCount > MDEF_MAX_INIT_BLOBS on parse (never reads out of bounds)");

  ControllerBuilder b = makeController();
  b.initBlobs.push_back({0xF0, 0xF7});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(blob[4] == 3);
  blob[13] = MDEF_MAX_INIT_BLOBS + 1;  // initCount header field, right before the name

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

void test_init_blob_len_overrun_rejected() {
  TEST("Reject an init blob whose declared length runs past the buffer end");

  ControllerBuilder b = makeController();
  b.initBlobs.push_back({0xF0, 0xF7});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(blob.empty() == false);

  // Truncate the blob right after the init table's length byte, so the
  // declared 2 bytes of payload have nowhere to come from.
  size_t nameOff = 14;  // 13-byte header + initCount(1)
  size_t afterName = nameOff + b.name.size();
  size_t padTable = afterName + 4u * b.pads.size();
  size_t faderTable = padTable + 6u * b.faders.size();
  size_t encoderTable = faderTable + 3u * b.encoders.size();
  size_t ledTable = encoderTable + 9u * b.leds.size();
  size_t colorTable = ledTable + 3u * 3 /* colors in makeController's LED */;
  size_t initLenByteOffset = colorTable;
  CHECK(blob[initLenByteOffset] == 2);  // the 2-byte {0xF0,0xF7} blob's length prefix
  blob.resize(initLenByteOffset + 1);   // cut off right after the length byte

  MidiControllerProfile p;
  CHECK(!parseMidiController(blob.data(), blob.size(), p));
}

int main() {
  test_roundtrip_basic_fields();
  test_led_range_lookup_by_note();
  test_color_lookup_by_name();
  test_single_pad();
  test_encoder_modes_parsed();
  test_decode_encoder_delta();
  test_bad_magic();
  test_bad_version();
  test_bad_flags();
  test_truncated_buffer();
  test_header_too_short();
  test_pad_count_overflow();
  test_bad_midi_channel();
  test_pad_from_after_to();
  test_led_bad_colorOffset();
  test_encoder_builder_limits();

  test_no_ch_stays_v1_byte_identical();
  test_one_ch_range_bumps_to_v2();
  test_ch_0_7_is_8_distinct_logical_pads();
  test_old_v1_blob_loads_as_agnostic();
  test_channel_range_validation();
  test_resolve_pad_xy_grid();

  test_init_blobs_roundtrip();
  test_init_blobs_with_channel_ranges_also_v3();
  test_no_init_line_sends_nothing();
  test_old_v1_v2_blobs_load_with_no_init();
  test_init_blob_count_limit();
  test_init_blob_size_limit();
  test_init_blob_count_overrun_rejected();
  test_init_blob_len_overrun_rejected();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
