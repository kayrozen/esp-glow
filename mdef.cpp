#include "mdef.h"

#include <cstring>

namespace {

bool hasNulWithin(const uint8_t* data, size_t start, size_t len) {
  const void* found = std::memchr(data + start, 0, len);
  return found != nullptr;
}

}  // namespace

bool parseMidiController(const uint8_t* data, size_t len, MidiControllerProfile& out) {
  if (len < 13) return false;

  if (data[0] != 'M' || data[1] != 'D' || data[2] != 'F' || data[3] != '1') return false;

  uint8_t version = data[4];
  if (version != 1) return false;

  uint8_t flags = data[5];
  if (flags != 0) return false;

  uint8_t midiChannel = data[6];
  if (midiChannel > 16) return false;

  uint8_t nameLen = data[7];
  uint8_t padCount = data[8];
  uint8_t faderCount = data[9];
  uint8_t encoderCount = data[10];
  uint8_t ledCount = data[11];
  uint8_t colorCount = data[12];

  if (padCount > MDEF_MAX_PADS) return false;
  if (faderCount > MDEF_MAX_FADERS) return false;
  if (encoderCount > MDEF_MAX_ENCODERS) return false;
  if (ledCount > MDEF_MAX_LED_RANGES) return false;
  if (colorCount > MDEF_MAX_COLORS) return false;

  size_t pos = 13;
  if (len < pos + nameLen) return false;
  pos += nameLen;  // controller name: parsed over, never stored (see mdef.h)

  size_t padTableOffset = pos;
  size_t faderTableOffset = padTableOffset + 2u * padCount;
  size_t encoderTableOffset = faderTableOffset + 4u * faderCount;
  size_t ledTableOffset = encoderTableOffset + 3u * encoderCount;
  size_t colorTableOffset = ledTableOffset + 7u * ledCount;
  size_t nameBlobOffset = colorTableOffset + 3u * colorCount;

  if (len < nameBlobOffset) return false;
  size_t nameBlobLen = len - nameBlobOffset;
  if (nameBlobLen > static_cast<size_t>(MDEF_MAX_NAME_BLOB)) return false;

  MidiControllerProfile result;
  result.midiChannel = midiChannel;

  // Pads
  result.padCount = padCount;
  for (uint8_t i = 0; i < padCount; ++i) {
    size_t o = padTableOffset + 2u * i;
    uint8_t from = data[o];
    uint8_t to = data[o + 1];
    if (from > 127 || to > 127 || from > to) return false;
    result.pads[i].noteFrom = from;
    result.pads[i].noteTo = to;
  }

  // Faders
  result.faderCount = faderCount;
  for (uint8_t i = 0; i < faderCount; ++i) {
    size_t o = faderTableOffset + 4u * i;
    uint8_t from = data[o];
    uint8_t to = data[o + 1];
    uint16_t nameOff = static_cast<uint16_t>(data[o + 2]) | (static_cast<uint16_t>(data[o + 3]) << 8);
    if (from > 127 || to > 127 || from > to) return false;
    if (nameOff != 0xFFFF) {
      if (nameOff >= nameBlobLen) return false;
      if (!hasNulWithin(data, nameBlobOffset + nameOff, nameBlobLen - nameOff)) return false;
    }
    result.faders[i].ccFrom = from;
    result.faders[i].ccTo = to;
    result.faders[i].nameOff = nameOff;
  }

  // Encoders
  result.encoderCount = encoderCount;
  for (uint8_t i = 0; i < encoderCount; ++i) {
    size_t o = encoderTableOffset + 3u * i;
    uint8_t from = data[o];
    uint8_t to = data[o + 1];
    uint8_t mode = data[o + 2];
    if (from > 127 || to > 127 || from > to) return false;
    if (mode > static_cast<uint8_t>(EncoderMode::RelativeSignMag)) return false;
    result.encoders[i].ccFrom = from;
    result.encoders[i].ccTo = to;
    result.encoders[i].mode = static_cast<EncoderMode>(mode);
  }

  // LED ranges
  result.ledCount = ledCount;
  for (uint8_t i = 0; i < ledCount; ++i) {
    size_t o = ledTableOffset + 7u * i;
    uint8_t msgType = data[o];
    uint8_t addrFrom = data[o + 1];
    uint8_t addrTo = data[o + 2];
    uint8_t semantic = data[o + 3];
    uint16_t colorOffset = static_cast<uint16_t>(data[o + 4]) | (static_cast<uint16_t>(data[o + 5]) << 8);
    uint8_t rangeColorCount = data[o + 6];

    if (msgType > static_cast<uint8_t>(LedMsgType::Cc)) return false;
    if (addrFrom > 127 || addrTo > 127 || addrFrom > addrTo) return false;
    if (semantic > static_cast<uint8_t>(LedSemantic::Value)) return false;
    if (colorOffset > colorCount) return false;
    if (static_cast<size_t>(colorOffset) + rangeColorCount > colorCount) return false;

    result.leds[i].msgType = static_cast<LedMsgType>(msgType);
    result.leds[i].addrFrom = addrFrom;
    result.leds[i].addrTo = addrTo;
    result.leds[i].semantic = static_cast<LedSemantic>(semantic);
    result.leds[i].colorOffset = colorOffset;
    result.leds[i].colorCount = rangeColorCount;
  }

  // Colours
  result.colorCount = colorCount;
  for (uint8_t i = 0; i < colorCount; ++i) {
    size_t o = colorTableOffset + 3u * i;
    uint16_t nameOff = static_cast<uint16_t>(data[o]) | (static_cast<uint16_t>(data[o + 1]) << 8);
    uint8_t value = data[o + 2];
    if (value > 127) return false;
    if (nameOff >= nameBlobLen) return false;
    if (!hasNulWithin(data, nameBlobOffset + nameOff, nameBlobLen - nameOff)) return false;
    result.colors[i].nameOff = nameOff;
    result.colors[i].value = value;
  }

  result.nameBlobLen = static_cast<uint16_t>(nameBlobLen);
  if (nameBlobLen > 0) {
    std::memcpy(result.nameBlob, data + nameBlobOffset, nameBlobLen);
  }

  out = result;
  return true;
}

const MdefLedRange* findLedRangeForNote(const MidiControllerProfile& p, uint8_t note) {
  for (uint8_t i = 0; i < p.ledCount; ++i) {
    const MdefLedRange& r = p.leds[i];
    if (r.msgType == LedMsgType::Note && note >= r.addrFrom && note <= r.addrTo) return &r;
  }
  return nullptr;
}

const MdefLedRange* findLedRangeForCc(const MidiControllerProfile& p, uint8_t cc) {
  for (uint8_t i = 0; i < p.ledCount; ++i) {
    const MdefLedRange& r = p.leds[i];
    if (r.msgType == LedMsgType::Cc && cc >= r.addrFrom && cc <= r.addrTo) return &r;
  }
  return nullptr;
}

bool ledColorValueByName(const MidiControllerProfile& p, const MdefLedRange& range,
                         const char* name, uint8_t& valueOut) {
  if (name == nullptr) return false;
  for (uint8_t i = 0; i < range.colorCount; ++i) {
    const MdefColorEntry& c = p.colors[range.colorOffset + i];
    const char* cname = reinterpret_cast<const char*>(&p.nameBlob[c.nameOff]);
    if (std::strcmp(cname, name) == 0) {
      valueOut = c.value;
      return true;
    }
  }
  return false;
}

int8_t decodeEncoderDelta(EncoderMode mode, uint8_t data2) {
  data2 &= 0x7F;  // MIDI data bytes are 7-bit
  switch (mode) {
    case EncoderMode::Relative2C:
      // Two's complement in 7 bits: bit6 set => negative (value - 128).
      return (data2 & 0x40) ? static_cast<int8_t>(data2 - 128) : static_cast<int8_t>(data2);
    case EncoderMode::RelativeSignMag: {
      // bit6 is the sign, low 6 bits are the magnitude.
      uint8_t magnitude = data2 & 0x3F;
      return (data2 & 0x40) ? static_cast<int8_t>(-static_cast<int>(magnitude))
                            : static_cast<int8_t>(magnitude);
    }
    case EncoderMode::Absolute:
    default:
      return 0;
  }
}
