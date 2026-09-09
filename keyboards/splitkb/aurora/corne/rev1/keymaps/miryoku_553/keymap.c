#include "quantum.h"
#include "gpio.h"
#include "raw_hid.h"
#include "oled_driver.h"
#include "transactions.h"
#include "split_util.h"
#include "atomic_util.h"
#include "caps_word.h"
#include "manna-harbour_miryoku.h"
#include "corne_oled.h"
#include <stddef.h>
#include <string.h>

#define HOST_STALE_MS 5000
#define DIM_MS 15000
#define OFF_MS 30000
#define AGE_MAX 60000
#define SYNC_INTERVAL_MS 80
#define SYNC_HEARTBEAT_MS 1000
#define LINK_STALE_MS 2500
#define REACTION_MS 650

enum host_flags { MIC_KNOWN = 1, MIC_MUTED = 2, OUTPUT_KNOWN = 4, OUTPUT_MUTED = 8, WORKSPACE_KNOWN = 16, MEDIA_KNOWN = 32 };
enum ui_flags { HOST_SEEN = 1, CAPS_WORD_ON = 2 };

// Ages saturate rather than wrapping back to "fresh" after a long disconnect.
// The slave advances received ages on its own clock; a heartbeat is not input.
typedef struct __attribute__((packed)) {
    uint8_t host[32];
    uint16_t host_age;
    uint16_t input_age;
    uint16_t key_age;
    uint8_t version;
    uint8_t layer;
    uint8_t held_mods;
    uint8_t oneshot_mods;
    uint8_t flags;
    uint8_t leds;
    uint8_t reaction;
} oled_state_t;

_Static_assert(sizeof(oled_state_t) <= 64, "OLED snapshot exceeds split RPC buffer");
static oled_state_t state;
static oled_state_t incoming;
static volatile bool incoming_ready;
static uint32_t incoming_at;
static uint32_t age_tick;
static uint32_t last_input_at;
static uint32_t last_sync;
static uint16_t link_age = AGE_MAX;
static bool link_seen;
static bool sync_dirty = true;
static bool sync_failed;
static bool render_dirty = true;

void keyboard_pre_init_user(void) {
    // The Liatris power LED is active-low.
    gpio_set_pin_output(24);
    gpio_write_pin_high(24);
}

static uint16_t age_add(uint16_t age, uint32_t elapsed) {
    return elapsed >= (uint32_t)AGE_MAX - age ? AGE_MAX : age + elapsed;
}

static void advance_ages(uint32_t now) {
    uint32_t elapsed = now - age_tick;
    age_tick = now;
    state.host_age = age_add(state.host_age, elapsed);
    state.input_age = age_add(state.input_age, elapsed);
    state.key_age = age_add(state.key_age, elapsed);
    link_age = age_add(link_age, elapsed);
}

static bool canonical_label(const uint8_t *label, uint8_t length, bool known, bool nonempty) {
    if (known && nonempty && label[0] == 0) {
        return false;
    }
    bool ended = !known;
    for (uint8_t i = 0; i < length; ++i) {
        if (label[i] == 0) {
            ended = true;
        } else if (ended || label[i] < 0x20 || label[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool valid_host_packet(const uint8_t *data, uint8_t length) {
    if (length != 32 || memcmp(data, "OLED", 4) != 0 || data[4] != 1) {
        return false;
    }
    uint8_t flags = data[5];
    if ((flags & 0xc0) || ((flags & MIC_MUTED) && !(flags & MIC_KNOWN)) ||
        ((flags & OUTPUT_MUTED) && !(flags & OUTPUT_KNOWN)) || data[6] > 100 || data[7] > 2 ||
        (!(flags & OUTPUT_KNOWN) && data[6] != 0) || (!(flags & MEDIA_KNOWN) && data[7] != 0)) {
        return false;
    }
    return canonical_label(data + 8, 8, flags & WORKSPACE_KNOWN, true) &&
           canonical_label(data + 16, 16, flags & MEDIA_KNOWN, false);
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    // A fixed, zero-padded report; access control belongs to Linux HID permissions.
    static const uint8_t bootloader_request[32] = "CORNE_BOOTLOADER_V1";
    if (length == sizeof(bootloader_request) && memcmp(data, bootloader_request, sizeof(bootloader_request)) == 0) {
        reset_keyboard();
        return;
    }
    if (!is_keyboard_master() || !valid_host_packet(data, length)) {
        return;
    }
    advance_ages(timer_read32());
    if (!(state.flags & HOST_SEEN) || state.host_age >= HOST_STALE_MS || memcmp(state.host, data, 32) != 0) {
        render_dirty = true;
    }
    memcpy(state.host, data, 32);
    state.flags |= HOST_SEEN;
    state.host_age = 0;
    sync_dirty = true;
}

static void receive_oled_state(uint8_t length, const void *data, uint8_t reply_length, void *reply) {
    (void)reply_length;
    (void)reply;
    if (length != sizeof(oled_state_t)) {
        return;
    }
    const oled_state_t *received = data;
    if (received->version != 1 || received->layer > U_GAMEFN || received->host_age > AGE_MAX || received->input_age > AGE_MAX ||
        received->key_age > AGE_MAX || received->reaction > 2) {
        return;
    }
    ATOMIC_BLOCK_RESTORESTATE {
        memcpy(&incoming, data, sizeof(incoming));
        incoming_at = timer_read32();
        incoming_ready = true;
    }
}

void corne_oled_init(void) {
    state.version = 1;
    state.host_age = AGE_MAX;
    state.key_age = AGE_MAX;
    age_tick = timer_read32();
    last_sync = age_tick;
    last_input_at = last_input_activity_time();
    transaction_register_rpc(CORNE_OLED_SYNC, receive_oled_state);
}

void corne_oled_keypress(uint16_t keycode) {
    if (!is_keyboard_master()) {
        return;
    }
    uint16_t tap = keycode;
    if (IS_QK_MOD_TAP(keycode)) {
        tap = QK_MOD_TAP_GET_TAP_KEYCODE(keycode);
    } else if (IS_QK_LAYER_TAP(keycode)) {
        tap = QK_LAYER_TAP_GET_TAP_KEYCODE(keycode);
    }
    advance_ages(timer_read32());
    state.input_age = 0;
    state.key_age = 0;
    state.reaction = tap == KC_SPC ? 1 : 2;
    sync_dirty = true;
    render_dirty = true;
}

static void sample_keyboard_state(void) {
    uint8_t flags = state.flags & HOST_SEEN;
    if (is_caps_word_on()) {
        flags |= CAPS_WORD_ON;
    }
    // An armed one-shot CZ layer is already the highest active layer, so it needs no flag.
    uint8_t layer = get_highest_layer(layer_state | default_layer_state);
    uint8_t held_mods = get_mods();
    uint8_t oneshot_mods = get_oneshot_mods();
    uint8_t leds = host_keyboard_led_state().raw;
    if (state.layer != layer || state.held_mods != held_mods || state.oneshot_mods != oneshot_mods || state.flags != flags || state.leds != leds) {
        state.layer = layer;
        state.held_mods = held_mods;
        state.oneshot_mods = oneshot_mods;
        state.flags = flags;
        state.leds = leds;
        sync_dirty = true;
        render_dirty = true;
    }
}

void housekeeping_task_user(void) {
    uint32_t now = timer_read32();
    advance_ages(now);
    if (!is_keyboard_master()) {
        bool received = false;
        uint32_t received_at = now;
        ATOMIC_BLOCK_RESTORESTATE {
            if (incoming_ready) {
                // Do not redraw on age-only heartbeats.
                render_dirty |= memcmp(state.host, incoming.host, sizeof(state.host)) != 0 ||
                                memcmp(&state.version, &incoming.version, sizeof(state) - offsetof(oled_state_t, version)) != 0 ||
                                (state.host_age >= HOST_STALE_MS) != (incoming.host_age >= HOST_STALE_MS);
                memcpy(&state, &incoming, sizeof(state));
                received_at = incoming_at;
                incoming_ready = false;
                received = true;
            }
        }
        if (received) {
            age_tick = received_at;
            link_age = 0;
            link_seen = true;
            advance_ages(timer_read32());
        }
        return;
    }

    uint32_t input_at = last_input_activity_time();
    if (input_at != last_input_at) {
        last_input_at = input_at;
        state.input_age = 0;
        sync_dirty = true;
    }
    sample_keyboard_state();
    uint32_t elapsed = now - last_sync;
    uint16_t interval = sync_failed ? SYNC_HEARTBEAT_MS : SYNC_INTERVAL_MS;
    if (elapsed >= interval && (sync_dirty || elapsed >= SYNC_HEARTBEAT_MS) && is_transport_connected()) {
        last_sync = now;
        // One bounded attempt; failures back off instead of burdening typing scans.
        sync_failed = !transaction_rpc_send(CORNE_OLED_SYNC, sizeof(state), &state);
        if (!sync_failed) {
            sync_dirty = false;
        }
    }
}

oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    (void)rotation;
    return OLED_ROTATION_270;
}

// Rendering: the whole 32x128 portrait panel is composed into a pixel framebuffer
// and flushed with one oled_write_raw, which only marks changed blocks dirty.
// Layout, top to bottom (pixel rows):
//   0-13  layer badge, three 2x letters
//   15-22 hairline, replaced by a lock label while Caps Word or a lock LED is on
//   24-32 modifier cells C S A G: dot idle, filled held, outlined one-shot
//   37-72 this hand's keys for the active layer, one glyph per key, thumbs below
//   76-   right: media block on typing layers   left: (blank)
//   108-  left: mic, output, workspace, level bar   right: 16x16 companion

extern const unsigned char font[] PROGMEM;

#define PANEL_WIDTH 32
#define Y_STATUS 15
#define Y_MODS 24
#define Y_MAP 37
#define Y_THUMBS (Y_MAP + 29)
#define Y_MEDIA_RULE 76
#define Y_MEDIA 80
#define Y_FOOTER 108
#define Y_BAR 120
#define SPRITE_PAGE 14

static uint8_t frame[OLED_MATRIX_SIZE];
static uint8_t previous_visual = 0xff;
static uint8_t previous_power = 0xff;

static void fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    for (uint8_t row = y; row < y + h; ++row) {
        uint8_t *page = frame + (row >> 3) * PANEL_WIDTH;
        uint8_t bit = 1 << (row & 7);
        for (uint8_t col = x; col < x + w; ++col) {
            page[col] |= bit;
        }
    }
}

static void outline(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    fill(x, y, w, 1);
    fill(x, y + h - 1, w, 1);
    fill(x, y, 1, h);
    fill(x + w - 1, y, 1, h);
}

// Eight vertical pixels at (x, y..y+7), bit 0 on top; y need not be page aligned.
static void column(uint8_t x, uint8_t y, uint8_t bits, bool on) {
    uint16_t shifted = (uint16_t)bits << (y & 7);
    uint16_t index = (y >> 3) * PANEL_WIDTH + x;
    if (on) {
        frame[index] |= shifted;
        if (index + PANEL_WIDTH < OLED_MATRIX_SIZE) {
            frame[index + PANEL_WIDTH] |= shifted >> 8;
        }
    } else {
        frame[index] &= ~shifted;
        if (index + PANEL_WIDTH < OLED_MATRIX_SIZE) {
            frame[index + PANEL_WIDTH] &= ~(shifted >> 8);
        }
    }
}

static void glyph(uint8_t x, uint8_t y, const uint8_t *columns, uint8_t width, bool on) {
    for (uint8_t col = 0; col < width; ++col) {
        column(x + col, y, pgm_read_byte(columns + col), on);
    }
}

static void letter(uint8_t x, uint8_t y, uint8_t code, bool on) {
    glyph(x, y, (const uint8_t *)font + code * OLED_FONT_WIDTH, 5, on);
}

static void text(uint8_t x, uint8_t y, const char *string) {
    for (uint8_t code; (code = pgm_read_byte(string)) != 0; ++string, x += 6) {
        letter(x, y, code, true);
    }
}

#define TEXT(x, y, literal) text(x, y, PSTR(literal))

static void badge(const char *name) {
    uint8_t length = strlen_P(name);
    uint8_t x = length == 3 ? 0 : 6;
    for (uint8_t i = 0; i < length; ++i, x += 11) {
        const uint8_t *columns = (const uint8_t *)font + pgm_read_byte(name + i) * OLED_FONT_WIDTH;
        for (uint8_t col = 0; col < 5; ++col) {
            uint8_t bits = pgm_read_byte(columns + col);
            for (uint8_t row = 0; row < 7; ++row) {
                if (bits & (1 << row)) {
                    fill(x + col * 2, row * 2, 2, 2);
                }
            }
        }
    }
}

static const char badges[U_GAMEFN + 1][4] PROGMEM = {
    [U_BASE] = "QWE", [U_EXTRA] = "CMK", [U_TAP] = "TAP", [U_BUTTON] = "BTN", [U_NAV] = "NAV",
    [U_MOUSE] = "MOU", [U_MEDIA] = "MED", [U_NUM] = "NUM", [U_SYM] = "SYM", [U_FUN] = "FUN",
    [U_GAME] = "GAM", [U_GAMENUM] = "GNM", [U_CZ] = "CZ", [U_GAMEFN] = "BNK"
};

enum icon { IC_SPC, IC_ENT, IC_BSPC, IC_DEL, IC_TAB, IC_ESC, IC_CW, IC_F10, IC_F11, IC_F12, IC_PAUS, IC_MENU, IC_STOP, IC_MUTE, IC_E_C, IC_E_A, IC_R_C, IC_T_C, IC_Y_A, IC_U_R, IC_I_A, IC_O_A, IC_A_A, IC_S_C, IC_D_C, IC_U_A, IC_Z_C, IC_C_C, IC_N_C, IC_SHFT };

// Column-major 5x7 glyphs, bit 0 is the top row. Czech letters are the font's lowercase with an accent composited in rows 0-1.
static const uint8_t icons[][5] PROGMEM = {
    [IC_SPC] = {0x30, 0x40, 0x40, 0x40, 0x30},
    [IC_ENT] = {0x10, 0x38, 0x10, 0x10, 0x1e},
    [IC_BSPC] = {0x08, 0x1c, 0x3e, 0x2a, 0x3e},
    [IC_DEL] = {0x3e, 0x2a, 0x3e, 0x1c, 0x08},
    [IC_TAB] = {0x08, 0x2a, 0x1c, 0x08, 0x3e},
    [IC_ESC] = {0x3c, 0x42, 0x42, 0x24, 0x1f},
    [IC_CW] = {0x7c, 0x52, 0x51, 0x52, 0x7c},
    [IC_F10] = {0x7f, 0x00, 0x7f, 0x41, 0x7f},
    [IC_F11] = {0x00, 0x7f, 0x00, 0x7f, 0x00},
    [IC_F12] = {0x7f, 0x00, 0x79, 0x49, 0x4f},
    [IC_PAUS] = {0x3e, 0x3e, 0x00, 0x3e, 0x3e},
    [IC_MENU] = {0x2a, 0x2a, 0x2a, 0x2a, 0x2a},
    [IC_STOP] = {0x00, 0x3e, 0x3e, 0x3e, 0x00},
    [IC_MUTE] = {0x5c, 0x3c, 0x3e, 0x2a, 0x45},
    [IC_E_C] = {0x38, 0x55, 0x56, 0x55, 0x18},
    [IC_E_A] = {0x38, 0x54, 0x56, 0x55, 0x18},
    [IC_R_C] = {0x7c, 0x09, 0x06, 0x05, 0x08},
    [IC_T_C] = {0x04, 0x3f, 0x44, 0x41, 0x20},
    [IC_Y_A] = {0x4c, 0x90, 0x92, 0x91, 0x7c},
    [IC_U_R] = {0x3c, 0x40, 0x41, 0x20, 0x7c},
    [IC_I_A] = {0x00, 0x44, 0x7e, 0x41, 0x00},
    [IC_O_A] = {0x38, 0x44, 0x46, 0x45, 0x38},
    [IC_A_A] = {0x20, 0x54, 0x56, 0x79, 0x40},
    [IC_S_C] = {0x48, 0x55, 0x56, 0x55, 0x24},
    [IC_D_C] = {0x38, 0x44, 0x44, 0x7f, 0x03},
    [IC_U_A] = {0x3c, 0x40, 0x42, 0x21, 0x7c},
    [IC_Z_C] = {0x44, 0x65, 0x56, 0x4d, 0x44},
    [IC_C_C] = {0x38, 0x45, 0x46, 0x45, 0x28},
    [IC_N_C] = {0x7c, 0x09, 0x06, 0x05, 0x78},
    [IC_SHFT] = {0x04, 0x06, 0x3f, 0x06, 0x04},
};

// Map cells: ' ' is empty, other values below 0x80 are font glyphs, ICON(i) selects icons[i].
#define ICON(index) (0x80 | (index))
#define ___ ' '

// One glyph per key: three rows of five, then the three thumbs (outer to inner on the left, inner to outer on the right).
// Cells mirror the Miryoku sources; layer-lock tap dances are left blank on purpose.
static const uint8_t maps[U_GAMEFN + 1][2][18] PROGMEM = {
    [U_BASE] = {
        {'Q', 'W', 'E', 'R', 'T',
         'A', 'S', 'D', 'F', 'G',
         'Z', 'X', 'C', 'V', 'B',
         ICON(IC_ESC), ICON(IC_SPC), ICON(IC_TAB)},
        {'Y', 'U', 'I', 'O', 'P',
         'H', 'J', 'K', 'L', '\'',
         'N', 'M', ',', '.', '/',
         ICON(IC_ENT), ICON(IC_BSPC), ICON(IC_DEL)},
    },
    [U_EXTRA] = {
        {'Q', 'W', 'F', 'P', 'B',
         'A', 'R', 'S', 'T', 'G',
         'Z', 'X', 'C', 'D', 'V',
         ICON(IC_ESC), ICON(IC_SPC), ICON(IC_TAB)},
        {'J', 'L', 'U', 'Y', '\'',
         'M', 'N', 'E', 'I', 'O',
         'K', 'H', ',', '.', '/',
         ICON(IC_ENT), ICON(IC_BSPC), ICON(IC_DEL)},
    },
    [U_TAP] = {
        {'Q', 'W', 'E', 'R', 'T',
         'A', 'S', 'D', 'F', 'G',
         'Z', 'X', 'C', 'V', 'B',
         ICON(IC_ESC), ICON(IC_SPC), ICON(IC_TAB)},
        {'Y', 'U', 'I', 'O', 'P',
         'H', 'J', 'K', 'L', '\'',
         'N', 'M', ',', '.', '/',
         ICON(IC_ENT), ICON(IC_BSPC), ICON(IC_DEL)},
    },
    [U_BUTTON] = {
        {'Z', 'X', 'C', 'V', 'Y',
         'G', 'A', 'C', 'S', ___,
         'Z', 'X', 'C', 'V', 'Y',
         '3', '1', '2'},
        {'Y', 'V', 'C', 'X', 'Z',
         ___, 'S', 'C', 'A', 'G',
         'Y', 'V', 'C', 'X', 'Z',
         '2', '1', '3'},
    },
    [U_NAV] = {
        {___, ___, ___, ___, ___,
         'G', 'A', 'C', 'S', ___,
         ___, 'R', ___, ___, ___,
         ___, ___, ___},
        {'Y', 'V', 'C', 'X', 'Z',
         0x1b, 0x19, 0x18, 0x1a, ICON(IC_CW),
         0x7f, 0x1f, 0x1e, '$', 'I',
         ICON(IC_ENT), ICON(IC_BSPC), ICON(IC_DEL)},
    },
    [U_MOUSE] = {
        {___, ___, ___, ___, ___,
         'G', 'A', 'C', 'S', ___,
         ___, 'R', ___, ___, ___,
         ___, ___, ___},
        {'Y', 'V', 'C', 'X', 'Z',
         0x1b, 0x19, 0x18, 0x1a, ___,
         0x11, 0x1f, 0x1e, 0x10, ___,
         '2', '1', '3'},
    },
    [U_MEDIA] = {
        {___, ___, ___, ___, ___,
         'G', 'A', 'C', 'S', ___,
         ___, 'R', ___, ___, ___,
         ___, ___, ___},
        {'M', 'H', 'S', 'V', '*',
         0x11, '-', '+', 0x10, ___,
         ___, ___, ___, ___, ___,
         ICON(IC_STOP), 0x10, ICON(IC_MUTE)},
    },
    [U_NUM] = {
        {'[', '7', '8', '9', ']',
         ';', '4', '5', '6', '=',
         '`', '1', '2', '3', '\\',
         '.', '0', '-'},
        {___, ___, ___, ___, ___,
         ___, 'S', 'C', 'A', 'G',
         ___, ___, ___, 'R', ___,
         ___, ___, ___},
    },
    [U_SYM] = {
        {'{', '&', '*', '(', '}',
         ':', '$', '%', '^', '+',
         '~', '!', '@', '#', '|',
         '(', ')', '_'},
        {___, ___, ___, ___, ___,
         ___, 'S', 'C', 'A', 'G',
         ___, ___, ___, 'R', ___,
         ___, ___, ___},
    },
    [U_FUN] = {
        {ICON(IC_F12), '7', '8', '9', 'P',
         ICON(IC_F11), '4', '5', '6', 'S',
         ICON(IC_F10), '1', '2', '3', ICON(IC_PAUS),
         ICON(IC_MENU), ICON(IC_SPC), ICON(IC_TAB)},
        {___, ___, ___, ___, ___,
         ___, 'S', 'C', 'A', 'G',
         ___, ___, ___, 'R', ___,
         ___, ___, ___},
    },
    [U_GAME] = {
        {ICON(IC_TAB), 'Q', 'W', 'E', 'R',
         ICON(IC_SHFT), 'A', 'S', 'D', 'F',
         '^', 'Z', 'X', 'C', 'V',
         'A', ICON(IC_SPC), '#'},
        {'Y', 'U', 'I', 'O', 'P',
         'H', 'J', 'K', 'L', '\'',
         'N', 'M', ',', '.', '/',
         ICON(IC_ENT), ICON(IC_BSPC), ICON(IC_DEL)},
    },
    [U_GAMENUM] = {
        {ICON(IC_ESC), '1', '2', '3', 'T',
         ICON(IC_SHFT), '4', '5', '6', 'G',
         '^', '7', '8', '9', 'B',
         0x7f, ICON(IC_SPC), ___},
        {'[', '7', '8', '9', ']',
         '=', '4', '5', '6', ';',
         '\\', '1', '2', '3', '`',
         '-', '0', '.'},
    },
    [U_CZ] = {
        {___, ICON(IC_E_C), ICON(IC_E_A), ICON(IC_R_C), ICON(IC_T_C),
         ICON(IC_A_A), ICON(IC_S_C), ICON(IC_D_C), ___, ___,
         ICON(IC_Z_C), ___, ICON(IC_C_C), ___, ___,
         ___, ___, ___},
        {ICON(IC_Y_A), ICON(IC_U_R), ICON(IC_I_A), ICON(IC_O_A), ___,
         ___, ICON(IC_U_A), ___, ___, ___,
         ICON(IC_N_C), ___, ___, ___, ___,
         ___, ___, ___},
    },
    [U_GAMEFN] = {
        {ICON(IC_TAB), 'Q', 'W', 'E', 'R',
         ICON(IC_SHFT), 'A', 'S', 'D', 'F',
         '^', 'Z', 'X', 'C', 'V',
         'A', ICON(IC_SPC), '#'},
        {'Y', 'U', 'I', 'O', 'P',
         'H', 'J', 'K', 'L', '\'',
         'N', 'M', ',', '.', '/',
         ICON(IC_ENT), ICON(IC_BSPC), ICON(IC_DEL)},
    },
};

static void cell(uint8_t x, uint8_t y, uint8_t code) {
    if (code == ___) {
        return;
    }
    if (code & 0x80) {
        glyph(x, y, icons[code & 0x7f], 5, true);
    } else {
        letter(x, y, code, true);
    }
}

// 7x8 footer icons, column-major.
static const uint8_t microphone_icon[7] PROGMEM = {0x00, 0x98, 0x9f, 0xff, 0x9f, 0x98, 0x00};
static const uint8_t speaker_icon[7] PROGMEM = {0x3c, 0x3c, 0x7e, 0xff, 0x00, 0x24, 0x18};

static void footer_icon(uint8_t x, const uint8_t *columns, bool muted) {
    glyph(x, Y_FOOTER, columns, 7, true);
    if (muted) {
        for (uint8_t i = 0; i < 8; ++i) {
            fill(x + i, Y_FOOTER + 7 - i, 1, 1);
        }
    }
}

// Column-major 16x16 pixels, two OLED pages per sprite. No idle animation.
static const uint8_t companion[][32] PROGMEM = {
    {0x00,0x00,0xf0,0x0c,0x02,0x1c,0x08,0x08,0x08,0x08,0x1c,0x02,0x0c,0xf0,0x00,0x00,
     0x00,0x00,0x07,0x08,0x12,0x12,0x10,0x14,0x14,0x10,0x12,0x12,0x08,0x07,0x00,0x00},
    {0x00,0x00,0xf0,0x0c,0x02,0x1c,0x08,0x08,0x08,0x08,0x1c,0x02,0x0c,0xf0,0x00,0x00,
     0x02,0x01,0x07,0x08,0x13,0x13,0x10,0x16,0x16,0x10,0x13,0x13,0x08,0x07,0x01,0x02},
    {0x00,0x00,0xf0,0x0c,0x02,0x1c,0x08,0x08,0x08,0x08,0x1c,0x02,0x0c,0xf0,0x00,0x00,
     0x04,0x02,0x07,0x08,0x11,0x12,0x10,0x14,0x14,0x10,0x12,0x11,0x08,0x07,0x02,0x04}
};

static bool host_fresh(void) {
    return (state.flags & HOST_SEEN) && state.host_age < HOST_STALE_MS;
}

static bool typing_layer(void) {
    return state.layer == U_BASE || state.layer == U_EXTRA || state.layer == U_TAP;
}

static void draw_status_line(void) {
    led_t leds = {.raw = state.leds};
    if (state.flags & CAPS_WORD_ON) {
        TEXT(4, Y_STATUS, "word");
    } else if (leds.caps_lock) {
        TEXT(4, Y_STATUS, "CAPS");
    } else if (leds.scroll_lock) {
        TEXT(4, Y_STATUS, "scrl");
    } else if (leds.num_lock) {
        TEXT(7, Y_STATUS, "num");
    } else {
        fill(0, Y_STATUS + 4, PANEL_WIDTH, 1);
    }
}

static void draw_mods(void) {
    static const uint8_t masks[4] = {MOD_MASK_CTRL, MOD_MASK_SHIFT, MOD_MASK_ALT, MOD_MASK_GUI};
    static const char letters[] = "CSAG";
    for (uint8_t i = 0; i < 4; ++i) {
        uint8_t x = i * 8;
        if (state.held_mods & masks[i]) {
            fill(x, Y_MODS, 7, 9);
            letter(x + 1, Y_MODS + 1, letters[i], false);
        } else if (state.oneshot_mods & masks[i]) {
            outline(x, Y_MODS, 7, 9);
            letter(x + 1, Y_MODS + 1, letters[i], true);
        } else {
            fill(x + 3, Y_MODS + 4, 1, 1);
        }
    }
}

static void draw_map(bool left) {
    const uint8_t *map = maps[state.layer][left ? 0 : 1];
    for (uint8_t row = 0; row < 3; ++row) {
        for (uint8_t col = 0; col < 5; ++col) {
            cell(1 + col * 6, Y_MAP + row * 9, pgm_read_byte(map + row * 5 + col));
        }
    }
    uint8_t x = left ? 13 : 1;
    for (uint8_t i = 0; i < 3; ++i) {
        cell(x + i * 6, Y_THUMBS, pgm_read_byte(map + 15 + i));
    }
}

static void draw_host_footer(void) {
    if (!host_fresh()) {
        return;
    }
    uint8_t flags = state.host[5];
    if (flags & MIC_KNOWN) {
        // Reflects the default source's mute state, never app recording.
        footer_icon(1, microphone_icon, flags & MIC_MUTED);
    }
    if (flags & OUTPUT_KNOWN) {
        footer_icon(10, speaker_icon, flags & OUTPUT_MUTED);
        outline(1, Y_BAR, 30, 5);
        uint8_t level = (uint16_t)state.host[6] * 28 / 100;
        if (level && !(flags & OUTPUT_MUTED)) {
            fill(2, Y_BAR + 1, level, 3);
        }
    }
    if (flags & WORKSPACE_KNOWN) {
        for (uint8_t i = 0; i < 2 && state.host[8 + i]; ++i) {
            letter(20 + i * 6, Y_FOOTER, state.host[8 + i], true);
        }
    }
}

static void draw_media(void) {
    if (!host_fresh() || !(state.host[5] & MEDIA_KNOWN)) {
        return;
    }
    fill(0, Y_MEDIA_RULE, PANEL_WIDTH, 1);
    uint8_t playback = state.host[7];
    if (playback == 2) {
        letter(1, Y_MEDIA, 0x10, true);
    } else {
        glyph(1, Y_MEDIA, icons[playback == 1 ? IC_PAUS : IC_STOP], 5, true);
    }
    // 14 characters as 4 + 5 + 5; the title comes first from the host, so the cut lands on the artist.
    const uint8_t *title = state.host + 16;
    for (uint8_t i = 0; i < 14 && title[i]; ++i) {
        uint8_t row = i < 4 ? 0 : (i - 4) / 5 + 1;
        uint8_t col = i < 4 ? i : (i - 4) % 5;
        letter((row == 0 ? 8 : 1) + col * 6, Y_MEDIA + row * 9, title[i], true);
    }
}

static void draw_sprite(uint8_t sprite) {
    memcpy_P(frame + SPRITE_PAGE * PANEL_WIDTH + 8, companion[sprite], 16);
    memcpy_P(frame + (SPRITE_PAGE + 1) * PANEL_WIDTH + 8, companion[sprite] + 16, 16);
}

bool oled_task_user(void) {
    advance_ages(timer_read32());
    uint8_t power = state.input_age >= OFF_MS ? 2 : state.input_age >= DIM_MS ? 1 : 0;
    if (power != previous_power) {
        if (power == 2) {
            // The driver wakes on dirty blocks: flush pending work before off.
            oled_render_dirty(true);
            oled_off();
        } else {
            oled_set_brightness(power == 1 ? 8 : OLED_BRIGHTNESS);
            oled_on();
        }
        previous_power = power;
    }
    if (power == 2) {
        return false;
    }
    bool link_lost = !is_keyboard_master() && (!link_seen || link_age >= LINK_STALE_MS);
    uint8_t sprite = state.key_age < REACTION_MS ? state.reaction : 0;
    uint8_t visual = (host_fresh() ? 1 : 0) | (link_lost ? 2 : 0) | (sprite << 2);
    if (!render_dirty && visual == previous_visual) {
        return false;
    }
    previous_visual = visual;
    render_dirty = false;
    memset(frame, 0, sizeof(frame));
    if (link_lost) {
        TEXT(4, 55, "link");
        text(4, 64, link_seen ? PSTR("lost") : PSTR("wait"));
    } else {
        bool left = is_keyboard_left();
        badge(badges[state.layer]);
        draw_status_line();
        draw_mods();
        draw_map(left);
        if (left) {
            draw_host_footer();
        } else {
            if (typing_layer()) {
                draw_media();
            }
            draw_sprite(sprite);
        }
    }
    oled_set_cursor(0, 0);
    oled_write_raw((const char *)frame, sizeof(frame));
    return false;
}
