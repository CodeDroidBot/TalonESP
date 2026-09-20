#pragma once
#include <Arduino.h>

// ============================================================
//  Placeholder icon asset header.
//
//  The original ir_remote.h (Flipper-style compressed icon pack) wasn't
//  part of this project's uploaded sources, so ir_module.cpp had nothing
//  to #include and the build failed at that line. This is a minimal
//  stand-in: it defines the same types and the same named Icon objects
//  ir_module.cpp references (so every `&I_xxx` still compiles and links),
//  but with frame_count = 0 - meaning "no bitmap available". ir_module.cpp
//  already checks for that and falls back to its own hand-drawn vector
//  glyphs (see drawUniversalGlyph()'s fallback section), so the Universal
//  Remote screen still renders a distinct, reasonable icon for every
//  button - just not the original pixel-art bitmaps.
//
//  If you have the real ir_remote.h/.cpp (icon bitmap data + a real
//  compress_icon_decode implementation), drop them in to replace this
//  pair and you'll get the original artwork instead of the vector
//  fallback. No other file needs to change either way.
// ============================================================

struct Icon {
  uint16_t width;
  uint16_t height;
  uint8_t frame_count;      // 0 here = "not backed by real bitmap data"
  const uint8_t* const* frames;
};

// Opaque decoder handle - real implementation would hold a decompression
// scratch buffer; this placeholder doesn't need one.
struct CompressIcon {
  uint8_t* scratch;
  size_t scratch_size;
};

// ---- Icon objects referenced by ir_module.cpp ----
extern const Icon I_power_19x20,        I_power_hover_19x20;
extern const Icon I_off_19x20,          I_off_hover_19x20;
extern const Icon I_mute_19x20,         I_mute_hover_19x20;
extern const Icon I_volup_24x21,        I_volup_hover_24x21;
extern const Icon I_voldown_24x21,      I_voldown_hover_24x21;
extern const Icon I_ch_up_24x21,        I_ch_up_hover_24x21;
extern const Icon I_ch_down_24x21,      I_ch_down_hover_24x21;
extern const Icon I_play_19x20,         I_play_hover_19x20;
extern const Icon I_pause_19x20,        I_pause_hover_19x20;
extern const Icon I_prev_19x20,         I_prev_hover_19x20;
extern const Icon I_next_19x20,         I_next_hover_19x20;
extern const Icon I_plus_19x20,         I_plus_hover_19x20;
extern const Icon I_minus_19x20,        I_minus_hover_19x20;
extern const Icon I_red_19x20,          I_red_hover_19x20;
extern const Icon I_green_19x20,        I_green_hover_19x20;
extern const Icon I_blue_19x20,         I_blue_hover_19x20;
extern const Icon I_white_19x20,        I_white_hover_19x20;
extern const Icon I_mode_19x20,         I_mode_hover_19x20;
extern const Icon I_rotate_19x20,       I_rotate_hover_19x20;
extern const Icon I_timer_19x20,        I_timer_hover_19x20;
extern const Icon I_dry_19x20,          I_dry_hover_19x20;
extern const Icon I_max_24x23,          I_max_hover_24x23;
extern const Icon I_celsius_24x23,      I_celsius_hover_24x23;
extern const Icon I_AC;

// Small UI glyphs used elsewhere (file lists, signal picker, etc.)
extern const Icon I_settings_10px;
extern const Icon I_Ok_btn_9x9;
extern const Icon I_InfraredArrowUp_4x8;
extern const Icon I_InfraredArrowDown_4x8;
extern const Icon I_ButtonLeft_4x7;
extern const Icon I_ButtonRight_4x7;

// ---- Decoder API ir_module.cpp expects (matches its forward decls) ----
CompressIcon* compress_icon_alloc(size_t decode_buf_size);
void compress_icon_free(CompressIcon* instance);
void compress_icon_decode(CompressIcon* instance, const uint8_t* icon_data, uint8_t** output);