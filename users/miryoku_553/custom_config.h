// Copyright 2019 Manna Harbour
// https://github.com/manna-harbour/miryoku

// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with this program. If not, see <http://www.gnu.org/licenses/>.

#pragma once

#pragma once

#define XXX KC_NO

// FPS friendly tap layer
#define MIRYOKU_LAYER_GAME \
KC_TAB,            KC_Q,              KC_W,             KC_E,             KC_R,              KC_Y,             KC_U,             KC_I,             KC_O,              KC_P,             \
KC_LSFT,           KC_A,              KC_S,             KC_D,             KC_F,              KC_H,             KC_J,             KC_K,             KC_L,              KC_QUOT,           \
KC_LCTL,           KC_Z,              KC_X,             KC_C,             KC_V,              KC_N,             KC_M,             KC_COMMA,         KC_DOT,            KC_SLASH,         \
U_NP,              U_NP,              KC_LALT,          KC_SPC,           MO(U_GAMENUM),     KC_ENT,           KC_BSPC,          KC_DEL,           U_NP,              U_NP

#define MIRYOKU_LAYER_GAMENUM \
KC_ESC,            KC_1,              KC_2,              KC_3,             KC_T,             KC_LBRC,          KC_F7,            KC_F8,            KC_F9,             KC_RBRC,         \
KC_LSFT,           KC_4,              KC_5,              KC_6,             KC_G,             KC_EQL,           KC_F4,            KC_F5,            KC_F6,             KC_SCLN,         \
KC_LCTL,           KC_7,              KC_8,              KC_9,             KC_B,             KC_BSLS,          KC_F1,            KC_F2,            KC_F3,             KC_GRAVE,        \
U_NP,              U_NP,              DF(U_BASE),        KC_SPC,           KC_NO,            KC_MINUS,         KC_0,             KC_DOT,           U_NP,              U_NP

// Czech diacritics — Unicode (lowercase) accented letters for the CZ layer.
#define CZ_AA UC(0x00E1)  // á
#define CZ_EE UC(0x00E9)  // é
#define CZ_EH UC(0x011B)  // ě
#define CZ_II UC(0x00ED)  // í
#define CZ_OO UC(0x00F3)  // ó
#define CZ_UU UC(0x00FA)  // ú
#define CZ_UH UC(0x016F)  // ů
#define CZ_YY UC(0x00FD)  // ý
#define CZ_CC UC(0x010D)  // č
#define CZ_DD UC(0x010F)  // ď
#define CZ_NN UC(0x0148)  // ň
#define CZ_RR UC(0x0159)  // ř
#define CZ_SS UC(0x0161)  // š
#define CZ_TT UC(0x0165)  // ť
#define CZ_ZZ UC(0x017E)  // ž

// CZ layer: Czech accents in QWERTY positions, everything else transparent so
// it falls through to BASE (space/backspace/mods/other letters keep working).
// Toggled on/off with the V+M combo (see manna-harbour_miryoku.c). Capital
// accents aren't produced here — use the Super+D AI rewrite for those.
#define MIRYOKU_LAYER_CZ \
_______,  CZ_EH,    CZ_EE,    CZ_RR,    CZ_TT,         CZ_YY,    CZ_UH,    CZ_II,    CZ_OO,    _______, \
CZ_AA,    CZ_SS,    CZ_DD,    _______,  _______,       _______,  CZ_UU,    _______,  _______,  _______, \
CZ_ZZ,    _______,  CZ_CC,    _______,  _______,       CZ_NN,    _______,  _______,  _______,  _______, \
U_NP,     U_NP,     _______,  _______,  _______,       _______,  _______,  _______,  U_NP,     U_NP

#define MIRYOKU_LAYER_LIST \
MIRYOKU_X(BASE,   "Base") \
MIRYOKU_X(EXTRA,  "Extra") \
MIRYOKU_X(TAP,    "Tap") \
MIRYOKU_X(BUTTON, "Button") \
MIRYOKU_X(NAV,    "Nav") \
MIRYOKU_X(MOUSE,  "Mouse") \
MIRYOKU_X(MEDIA,  "Media") \
MIRYOKU_X(NUM,    "Num") \
MIRYOKU_X(SYM,    "Sym") \
MIRYOKU_X(FUN,    "Fun") \
MIRYOKU_X(GAME,   "Game") \
MIRYOKU_X(GAMENUM,"GNum") \
MIRYOKU_X(CZ,     "CZ") \
MIRYOKU_X(GAMEFN, "GFn")

#define MIRYOKU_LAYERMAPPING_BASE( \
      K00,  K01,  K02,  K03,  K04,         K05,  K06,  K07,  K08,  K09, \
      K10,  K11,  K12,  K13,  K14,         K15,  K16,  K17,  K18,  K19, \
      K20,  K21,  K22,  K23,  K24,         K25,  K26,  K27,  K28,  K29, \
      N30,  N31,  K32,  K33,  K34,         K35,  K36,  K37,  N38,  N39 \
) \
LAYOUT_split_3x6_3( \
XXX,  K00,  K01,  K02,  K03,  K04,         K05,  K06,  K07,  K08,  K09,  DF(U_GAME), \
XXX,  K10,  K11,  K12,  K13,  K14,         K15,  K16,  K17,  K18,  K19,  XXX, \
XXX,  K20,  K21,  K22,  K23,  K24,         K25,  K26,  K27,  K28,  K29,  XXX , \
                  K32,  K33,  K34,         K35,  K36,  K37 \
)
#define MIRYOKU_LAYERMAPPING_GAME( \
      K00,  K01,  K02,  K03,  K04,         K05,  K06,  K07,  K08,  K09, \
      K10,  K11,  K12,  K13,  K14,         K15,  K16,  K17,  K18,  K19, \
      K20,  K21,  K22,  K23,  K24,         K25,  K26,  K27,  K28,  K29, \
      N30,  N31,  K32,  K33,  K34,         K35,  K36,  K37,  N38,  N39 \
) \
LAYOUT_split_3x6_3( \
KC_F13,  K00,  K01,  K02,  K03,  K04,         K05,  K06,  K07,  K08,  K09,  DF(U_BASE), \
KC_F14,  K10,  K11,  K12,  K13,  K14,         K15,  K16,  K17,  K18,  K19,  KC_F16, \
KC_F15,  K20,  K21,  K22,  K23,  K24,         K25,  K26,  K27,  K28,  K29,  MO(U_GAMEFN), \
                  K32,  K33,  K34,         K35,  K36,  K37 \
)
#define MIRYOKU_LAYERMAPPING_GAMENUM MIRYOKU_MAPPING
#define MIRYOKU_LAYERMAPPING_CZ MIRYOKU_MAPPING

// GAME "bank" layer: hold the bank key (MO(U_GAMEFN), right-outer bottom on the
// GAME layer) to turn the four game F-keys F13-F16 into F17-F20. Inner keys stay
// transparent so normal game keys keep working. Cross-hand banking (hold the
// right-side bank + tap the left-column F13-F15) is conflict-free; F16->F20
// shares the right pinky, so reach F20 on its own.
#define MIRYOKU_LAYER_GAMEFN \
_______,  _______,  _______,  _______,  _______,       _______,  _______,  _______,  _______,  _______, \
_______,  _______,  _______,  _______,  _______,       _______,  _______,  _______,  _______,  _______, \
_______,  _______,  _______,  _______,  _______,       _______,  _______,  _______,  _______,  _______, \
U_NP,     U_NP,     _______,  _______,  _______,       _______,  _______,  _______,  U_NP,     U_NP

#define MIRYOKU_LAYERMAPPING_GAMEFN( \
      K00,  K01,  K02,  K03,  K04,         K05,  K06,  K07,  K08,  K09, \
      K10,  K11,  K12,  K13,  K14,         K15,  K16,  K17,  K18,  K19, \
      K20,  K21,  K22,  K23,  K24,         K25,  K26,  K27,  K28,  K29, \
      N30,  N31,  K32,  K33,  K34,         K35,  K36,  K37,  N38,  N39 \
) \
LAYOUT_split_3x6_3( \
KC_F17,  K00,  K01,  K02,  K03,  K04,         K05,  K06,  K07,  K08,  K09,  _______, \
KC_F18,  K10,  K11,  K12,  K13,  K14,         K15,  K16,  K17,  K18,  K19,  KC_F20, \
KC_F19,  K20,  K21,  K22,  K23,  K24,         K25,  K26,  K27,  K28,  K29,  _______, \
                  K32,  K33,  K34,         K35,  K36,  K37 \
)
