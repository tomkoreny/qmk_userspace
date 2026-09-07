// Copyright 2022 Manna Harbour
// https://github.com/manna-harbour/miryoku

// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with this program. If not, see <http://www.gnu.org/licenses/>.

#include QMK_KEYBOARD_H

#include "manna-harbour_miryoku.h"


// Additional Features double tap guard

enum {
    U_TD_BOOT,
#define MIRYOKU_X(LAYER, STRING) U_TD_U_##LAYER,
MIRYOKU_LAYER_LIST
#undef MIRYOKU_X
};

void u_td_fn_boot(tap_dance_state_t *state, void *user_data) {
  if (state->count == 2) {
    reset_keyboard();
  }
}

#define MIRYOKU_X(LAYER, STRING) \
void u_td_fn_U_##LAYER(tap_dance_state_t *state, void *user_data) { \
  if (state->count == 2) { \
    default_layer_set((layer_state_t)1 << U_##LAYER); \
  } \
}
MIRYOKU_LAYER_LIST
#undef MIRYOKU_X

tap_dance_action_t tap_dance_actions[] = {
    [U_TD_BOOT] = ACTION_TAP_DANCE_FN(u_td_fn_boot),
#define MIRYOKU_X(LAYER, STRING) [U_TD_U_##LAYER] = ACTION_TAP_DANCE_FN(u_td_fn_U_##LAYER),
MIRYOKU_LAYER_LIST
#undef MIRYOKU_X
};


// keymap

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
#define MIRYOKU_X(LAYER, STRING) [U_##LAYER] = U_MACRO_VA_ARGS(MIRYOKU_LAYERMAPPING_##LAYER, MIRYOKU_LAYER_##LAYER),
MIRYOKU_LAYER_LIST
#undef MIRYOKU_X
};


// shift functions

const key_override_t capsword_key_override = ko_make_basic(MOD_MASK_SHIFT, CW_TOGG, KC_CAPS);

const key_override_t *key_overrides[] = {
    &capsword_key_override,
};


// thumb combos

#if defined (MIRYOKU_KLUDGE_THUMBCOMBOS)
const uint16_t PROGMEM thumbcombos_base_right[] = {LT(U_SYM, KC_ENT), LT(U_NUM, KC_BSPC), COMBO_END};
const uint16_t PROGMEM thumbcombos_base_left[] = {LT(U_NAV, KC_SPC), LT(U_MOUSE, KC_TAB), COMBO_END};
const uint16_t PROGMEM thumbcombos_nav[] = {KC_ENT, KC_BSPC, COMBO_END};
const uint16_t PROGMEM thumbcombos_mouse[] = {KC_BTN2, KC_BTN1, COMBO_END};
const uint16_t PROGMEM thumbcombos_media[] = {KC_MSTP, KC_MPLY, COMBO_END};
const uint16_t PROGMEM thumbcombos_num[] = {KC_0, KC_MINS, COMBO_END};
  #if defined (MIRYOKU_LAYERS_FLIP)
const uint16_t PROGMEM thumbcombos_sym[] = {KC_UNDS, KC_LPRN, COMBO_END};
  #else
const uint16_t PROGMEM thumbcombos_sym[] = {KC_RPRN, KC_UNDS, COMBO_END};
  #endif
const uint16_t PROGMEM thumbcombos_fun[] = {KC_SPC, KC_TAB, COMBO_END};
combo_t key_combos[COMBO_COUNT] = {
  COMBO(thumbcombos_base_right, LT(U_FUN, KC_DEL)),
  COMBO(thumbcombos_base_left, LT(U_MEDIA, KC_ESC)),
  COMBO(thumbcombos_nav, KC_DEL),
  COMBO(thumbcombos_mouse, KC_BTN3),
  COMBO(thumbcombos_media, KC_MUTE),
  COMBO(thumbcombos_num, KC_DOT),
  #if defined (MIRYOKU_LAYERS_FLIP)
  COMBO(thumbcombos_sym, KC_RPRN),
  #else
  COMBO(thumbcombos_sym, KC_LPRN),
  #endif
  COMBO(thumbcombos_fun, KC_APP)
};
#endif


// Czech diacritics layer

// Default to the Linux Unicode input mode at boot (only writes EEPROM if it
// isn't already set, to avoid flash wear).
void keyboard_post_init_user(void) {
  if (get_unicode_input_mode() != UNICODE_MODE_LINUX) {
    set_unicode_input_mode(UNICODE_MODE_LINUX);
  }
}

// Custom keycode: toggle the default base layer between QWERTY (U_BASE) and
// Colemak-DH (U_EXTRA), for practising Colemak. The OLED shows "Extra" while in
// Colemak-DH.
enum custom_keycodes {
  CK_QWCM = QK_USER,
};

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
  if (keycode == CK_QWCM && record->event.pressed) {
    if (get_highest_layer(default_layer_state) == U_EXTRA) {
      default_layer_set((layer_state_t)1 << U_BASE);
    } else {
      default_layer_set((layer_state_t)1 << U_EXTRA);
    }
    return false;
  }
  return true;
}

// V + M arms the CZ Czech-accents layer for ONE keypress (one-shot); after the
// next letter it returns to the base layer automatically.
// C + , toggles QWERTY <-> Colemak-DH (both are plain keys on the same physical
// positions in either layout, so the same chord toggles both directions).
#if !defined (MIRYOKU_KLUDGE_THUMBCOMBOS)
const uint16_t PROGMEM cz_combo[] = {KC_V, KC_M, COMBO_END};
const uint16_t PROGMEM layout_combo[] = {KC_C, KC_COMM, COMBO_END};
const uint16_t PROGMEM caps_word_combo[] = {KC_C, KC_M, COMBO_END};
combo_t key_combos[] = {
  COMBO(cz_combo, OSL(U_CZ)),
  COMBO(layout_combo, CK_QWCM),
  COMBO(caps_word_combo, CW_TOGG),
};

bool combo_should_trigger(uint16_t combo_index, combo_t *combo, uint16_t keycode, keyrecord_t *record) {
  (void)combo_index;
  (void)combo;
  (void)keycode;
  (void)record;
  const layer_state_t typing_layers = ((layer_state_t)1 << U_BASE) | ((layer_state_t)1 << U_EXTRA) | ((layer_state_t)1 << U_TAP);
  const layer_state_t active_layers = layer_state | default_layer_state;
  return active_layers != 0 && (active_layers & ~typing_layers) == 0;
}
#endif
