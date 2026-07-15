#pragma once

#include "mdef.h"
#include <string>
#include <vector>

// Host-tool-only MDF1 encoder (mirrors profile_encoder.h's ProfileBuilder).
// Never linked into firmware -- mdef.h/mdef.cpp (parseMidiController) is
// the only piece of the format device code ever sees.

struct ControllerFaderSpec {
  uint8_t ccFrom = 0;
  uint8_t ccTo = 0;
  std::string name;  // empty = unnamed
  uint8_t channelFrom = kChannelAgnostic;
  uint8_t channelTo = kChannelAgnostic;  // both kChannelAgnostic = ordinary (v1-shaped)
};

struct ControllerEncoderSpec {
  uint8_t ccFrom = 0;
  uint8_t ccTo = 0;
  EncoderMode mode = EncoderMode::Absolute;
};

struct ControllerColorSpec {
  std::string name;
  uint8_t value = 0;
};

struct ControllerLedSpec {
  LedMsgType msgType = LedMsgType::Note;
  uint8_t addrFrom = 0;
  uint8_t addrTo = 0;
  LedSemantic semantic = LedSemantic::Velocity;
  std::vector<ControllerColorSpec> colors;
  uint8_t channelFrom = kChannelAgnostic;
  uint8_t channelTo = kChannelAgnostic;  // LED-output channel significance; see mdef.h's MdefLedRange
};

struct ControllerPadSpec {
  uint8_t noteFrom = 0;
  uint8_t noteTo = 0;
  uint8_t channelFrom = kChannelAgnostic;
  uint8_t channelTo = kChannelAgnostic;  // both kChannelAgnostic = ordinary (v1-shaped)
};

struct ControllerBuilder {
  std::string name;
  uint8_t midiChannel = 0;
  std::vector<ControllerPadSpec> pads;
  std::vector<ControllerFaderSpec> faders;
  std::vector<ControllerEncoderSpec> encoders;
  std::vector<ControllerLedSpec> leds;

  // Encodes an MDF1 blob. Fails (returns an empty vector) if any count
  // exceeds its MDEF_MAX_* or the combined name/colour text overruns
  // MDEF_MAX_NAME_BLOB -- the caller (provision.cpp's encodeController)
  // turns that into a proper CompileResult error instead of silently
  // shipping a truncated blob that parseMidiController would then reject.
  std::vector<uint8_t> encode(std::string& err) const;
};
