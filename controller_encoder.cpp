#include "controller_encoder.h"

#include <utility>

std::vector<uint8_t> ControllerBuilder::encode(std::string& err) const {
  err.clear();
  std::vector<uint8_t> blob;

  if (name.size() > 255) {
    err = "controller name too long (max 255 bytes)";
    return {};
  }
  if (pads.size() > static_cast<size_t>(MDEF_MAX_PADS)) {
    err = "too many PAD declarations (max " + std::to_string(MDEF_MAX_PADS) + ")";
    return {};
  }
  if (faders.size() > static_cast<size_t>(MDEF_MAX_FADERS)) {
    err = "too many FADER declarations (max " + std::to_string(MDEF_MAX_FADERS) + ")";
    return {};
  }
  if (encoders.size() > static_cast<size_t>(MDEF_MAX_ENCODERS)) {
    err = "too many ENCODER declarations (max " + std::to_string(MDEF_MAX_ENCODERS) + ")";
    return {};
  }
  if (leds.size() > static_cast<size_t>(MDEF_MAX_LED_RANGES)) {
    err = "too many LED declarations (max " + std::to_string(MDEF_MAX_LED_RANGES) + ")";
    return {};
  }
  size_t totalColors = 0;
  for (const auto& l : leds) totalColors += l.colors.size();
  if (totalColors > static_cast<size_t>(MDEF_MAX_COLORS)) {
    err = "too many COLOR entries across all LED ranges (max " + std::to_string(MDEF_MAX_COLORS) + ")";
    return {};
  }

  // Header
  blob.push_back('M');
  blob.push_back('D');
  blob.push_back('F');
  blob.push_back('1');
  blob.push_back(1);  // version
  blob.push_back(0);  // flags
  blob.push_back(midiChannel);
  blob.push_back(static_cast<uint8_t>(name.size()));
  blob.push_back(static_cast<uint8_t>(pads.size()));
  blob.push_back(static_cast<uint8_t>(faders.size()));
  blob.push_back(static_cast<uint8_t>(encoders.size()));
  blob.push_back(static_cast<uint8_t>(leds.size()));
  blob.push_back(static_cast<uint8_t>(totalColors));

  for (char c : name) blob.push_back(static_cast<uint8_t>(c));

  for (const auto& p : pads) {
    blob.push_back(p.noteFrom);
    blob.push_back(p.noteTo);
  }

  // Fader/colour names: one trailing NUL-separated blob, undeduplicated
  // (same convention as ProfileBuilder::encode's range-name blob).
  std::vector<uint8_t> nameBlob;
  std::vector<uint16_t> faderNameOffs;
  faderNameOffs.reserve(faders.size());
  for (const auto& f : faders) {
    if (f.name.empty()) {
      faderNameOffs.push_back(0xFFFF);
      continue;
    }
    faderNameOffs.push_back(static_cast<uint16_t>(nameBlob.size()));
    for (char c : f.name) nameBlob.push_back(static_cast<uint8_t>(c));
    nameBlob.push_back(0);
  }

  for (size_t i = 0; i < faders.size(); ++i) {
    const auto& f = faders[i];
    blob.push_back(f.ccFrom);
    blob.push_back(f.ccTo);
    blob.push_back(faderNameOffs[i] & 0xFF);
    blob.push_back((faderNameOffs[i] >> 8) & 0xFF);
  }

  for (const auto& e : encoders) {
    blob.push_back(e.ccFrom);
    blob.push_back(e.ccTo);
    blob.push_back(static_cast<uint8_t>(e.mode));
  }

  // LED records reference a contiguous slice of the global colour table in
  // declaration order; colour name offsets are assigned into the same
  // trailing blob as fader names.
  uint16_t colorCursor = 0;
  std::vector<std::pair<uint16_t, uint8_t>> colorTable;  // (nameOff, value)
  for (const auto& l : leds) {
    blob.push_back(static_cast<uint8_t>(l.msgType));
    blob.push_back(l.addrFrom);
    blob.push_back(l.addrTo);
    blob.push_back(static_cast<uint8_t>(l.semantic));
    blob.push_back(colorCursor & 0xFF);
    blob.push_back((colorCursor >> 8) & 0xFF);
    blob.push_back(static_cast<uint8_t>(l.colors.size()));
    colorCursor = static_cast<uint16_t>(colorCursor + l.colors.size());

    for (const auto& c : l.colors) {
      uint16_t off = static_cast<uint16_t>(nameBlob.size());
      for (char ch : c.name) nameBlob.push_back(static_cast<uint8_t>(ch));
      nameBlob.push_back(0);
      colorTable.emplace_back(off, c.value);
    }
  }

  for (const auto& c : colorTable) {
    blob.push_back(c.first & 0xFF);
    blob.push_back((c.first >> 8) & 0xFF);
    blob.push_back(c.second);
  }

  if (nameBlob.size() > static_cast<size_t>(MDEF_MAX_NAME_BLOB)) {
    err = "fader/colour name blob too large (max " + std::to_string(MDEF_MAX_NAME_BLOB) + " bytes)";
    return {};
  }

  for (uint8_t b : nameBlob) blob.push_back(b);

  return blob;
}
