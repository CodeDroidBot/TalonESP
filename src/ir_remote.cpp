#include "ir_remote.h"
#include <cstdlib>

// See ir_remote.h for why this file exists: it's a placeholder for the
// missing original icon asset pack, not a reimplementation of it.

// Every Icon below is intentionally empty (frame_count = 0, frames =
// nullptr). ir_module.cpp treats frame_count == 0 as "draw the vector
// fallback instead" (see drawUniversalGlyph() in ir_module.cpp), so these
// just need to exist with the right name and size for &I_xxx to compile -
// the actual pixel data is never read.
#define STUB_ICON(name, w, h) const Icon name = { (w), (h), 0, nullptr }

STUB_ICON(I_power_19x20, 19, 20);       STUB_ICON(I_power_hover_19x20, 19, 20);
STUB_ICON(I_off_19x20, 19, 20);         STUB_ICON(I_off_hover_19x20, 19, 20);
STUB_ICON(I_mute_19x20, 19, 20);        STUB_ICON(I_mute_hover_19x20, 19, 20);
STUB_ICON(I_volup_24x21, 24, 21);       STUB_ICON(I_volup_hover_24x21, 24, 21);
STUB_ICON(I_voldown_24x21, 24, 21);     STUB_ICON(I_voldown_hover_24x21, 24, 21);
STUB_ICON(I_ch_up_24x21, 24, 21);       STUB_ICON(I_ch_up_hover_24x21, 24, 21);
STUB_ICON(I_ch_down_24x21, 24, 21);     STUB_ICON(I_ch_down_hover_24x21, 24, 21);
STUB_ICON(I_play_19x20, 19, 20);        STUB_ICON(I_play_hover_19x20, 19, 20);
STUB_ICON(I_pause_19x20, 19, 20);       STUB_ICON(I_pause_hover_19x20, 19, 20);
STUB_ICON(I_prev_19x20, 19, 20);        STUB_ICON(I_prev_hover_19x20, 19, 20);
STUB_ICON(I_next_19x20, 19, 20);        STUB_ICON(I_next_hover_19x20, 19, 20);
STUB_ICON(I_plus_19x20, 19, 20);        STUB_ICON(I_plus_hover_19x20, 19, 20);
STUB_ICON(I_minus_19x20, 19, 20);       STUB_ICON(I_minus_hover_19x20, 19, 20);
STUB_ICON(I_red_19x20, 19, 20);         STUB_ICON(I_red_hover_19x20, 19, 20);
STUB_ICON(I_green_19x20, 19, 20);       STUB_ICON(I_green_hover_19x20, 19, 20);
STUB_ICON(I_blue_19x20, 19, 20);        STUB_ICON(I_blue_hover_19x20, 19, 20);
STUB_ICON(I_white_19x20, 19, 20);       STUB_ICON(I_white_hover_19x20, 19, 20);
STUB_ICON(I_mode_19x20, 19, 20);        STUB_ICON(I_mode_hover_19x20, 19, 20);
STUB_ICON(I_rotate_19x20, 19, 20);      STUB_ICON(I_rotate_hover_19x20, 19, 20);
STUB_ICON(I_timer_19x20, 19, 20);       STUB_ICON(I_timer_hover_19x20, 19, 20);
STUB_ICON(I_dry_19x20, 19, 20);         STUB_ICON(I_dry_hover_19x20, 19, 20);
STUB_ICON(I_max_24x23, 24, 23);         STUB_ICON(I_max_hover_24x23, 24, 23);
STUB_ICON(I_celsius_24x23, 24, 23);     STUB_ICON(I_celsius_hover_24x23, 24, 23);
STUB_ICON(I_AC, 24, 23);

STUB_ICON(I_settings_10px, 10, 10);
STUB_ICON(I_Ok_btn_9x9, 9, 9);
STUB_ICON(I_InfraredArrowUp_4x8, 4, 8);
STUB_ICON(I_InfraredArrowDown_4x8, 4, 8);
STUB_ICON(I_ButtonLeft_4x7, 4, 7);
STUB_ICON(I_ButtonRight_4x7, 4, 7);

#undef STUB_ICON

CompressIcon* compress_icon_alloc(size_t decode_buf_size) {
  CompressIcon* inst = (CompressIcon*)malloc(sizeof(CompressIcon));
  if (!inst) return nullptr;
  inst->scratch = (uint8_t*)malloc(decode_buf_size);
  inst->scratch_size = decode_buf_size;
  return inst;
}

void compress_icon_free(CompressIcon* instance) {
  if (!instance) return;
  free(instance->scratch);
  free(instance);
}

void compress_icon_decode(CompressIcon* instance, const uint8_t* icon_data, uint8_t** output) {
  // Never actually called in practice: every Icon here has frame_count==0,
  // and ir_module.cpp's drawUniversalGlyph() skips the irDrawIcon() call
  // entirely for those (see the frame_count check added there). This is
  // just here so the link succeeds if something ever does call it.
  (void)instance;
  (void)icon_data;
  if (output) *output = nullptr;
}