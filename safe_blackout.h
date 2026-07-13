// safe_blackout.h — F5: the one safe fallback state every failure path
// funnels into (corrupt/missing SHW1 bundle, boot.fnl error, a scripts
// partition that won't mount, an OTA image that fails self-validation).
//
// "Blackout" here means every fixture goes dark and every cue stops — NOT
// "stop rendering". A rig that stops transmitting DMX/Art-Net leaves
// downstream gear holding its last frame (many fixtures and every Art-Net
// bridge do this) — a stuck-on light is worse than a dark one. The render
// loop, DmxSink, and ArtNetSink must all keep running after blackout,
// continuously sending zeros, for as long as the device stays up.
#pragma once

#include "show.h"
#include "show_control.h"

// Host-tested core of F5's safe blackout (see glow_safe_blackout in
// main.cpp for the device-facing entry point, which additionally logs to
// serial and broadcasts a `blackout` message to the web console — see
// web_protocol.h's buildBlackoutJson).
//
// Stops every cue immediately (ShowController::stopAll() — no fade, unlike
// release()) and zeros every Raw-mode universe's pixel buffer directly.
// Fixture-mode universes need no explicit zeroing here: once no cue is
// active, ShowController::evaluate() emits no intents, and
// Show::renderFrame's per-frame pass (show.cpp) takes over on its own —
// every universe is zeroed, then every patched fixture's profile defaults
// (e.g. shutter open) are written back in, with nothing left to override
// them. Raw-mode universes are different: renderFrame never touches their
// contents at all — only writeRawUniverse does, driven by whatever pattern
// code calls it each frame — so they need an explicit zero here.
//
// IMPORTANT — this function alone does not keep a Raw universe dark: it
// zeros it ONCE. If something keeps calling writeRawUniverse into that same
// universe on later frames (e.g. a pixel-matrix pattern still running in
// main.cpp's on_pre_render), it will immediately overwrite the zero on the
// very next frame. The caller is responsible for also stopping whatever
// drives Raw universes before/alongside calling this (see main.cpp's
// g_blackout flag, checked by on_pre_render before it re-drives any
// matrix).
void safeBlackoutCore(Show& show, ShowController& controller);
