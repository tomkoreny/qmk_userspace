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

enum oled_page { PAGE_AUTO, PAGE_HINTS, PAGE_HOST, PAGE_PRACTICE, PAGE_COMPANION, PAGE_COUNT };
enum host_flags { MIC_KNOWN = 1, MIC_MUTED = 2, OUTPUT_KNOWN = 4, OUTPUT_MUTED = 8, WORKSPACE_KNOWN = 16, MEDIA_KNOWN = 32 };
enum ui_flags { HOST_SEEN = 1, CAPS_WORD_ON = 2, CZ_ARMED = 4, GAMING = 8, GAME_BANK = 16 };

// Ages saturate rather than wrapping back to "fresh" after a long disconnect.
// The slave advances received ages on its own clock; a heartbeat is not input.
typedef struct __attribute__((packed)) {
    uint8_t host[32];
    uint16_t host_age;
    uint16_t input_age;
    uint16_t key_age;
    uint8_t version;
    uint8_t layout;
    uint8_t layer;
    uint8_t held_mods;
    uint8_t oneshot_mods;
    uint8_t oneshot_layer;
    uint8_t flags;
    uint8_t leds;
    uint8_t page;
    uint8_t reaction;
    uint8_t key_serial;
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
    if (received->version != 1 || received->page >= PAGE_COUNT || received->layout > U_GAMEFN || received->layer > U_GAMEFN ||
        received->host_age > AGE_MAX || received->input_age > AGE_MAX || received->key_age > AGE_MAX || received->reaction > 2) {
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
    state.oneshot_layer = 0xff;
    age_tick = timer_read32();
    last_sync = age_tick;
    last_input_at = last_input_activity_time();
    transaction_register_rpc(CORNE_OLED_SYNC, receive_oled_state);
}

void corne_oled_keypress(uint16_t keycode) {
    if (!is_keyboard_master()) {
        return;
    }
    advance_ages(timer_read32());
    state.input_age = 0;
    state.key_age = 0;
    state.reaction = keycode == KC_SPC ? 1 : 2;
    ++state.key_serial;
    sync_dirty = true;
    render_dirty = true;
}

void corne_oled_cycle_page(void) {
    if (!is_keyboard_master()) {
        return;
    }
    corne_oled_keypress(KC_NO);
    state.page = (state.page + 1) % PAGE_COUNT;
}

static void sample_keyboard_state(void) {
    uint8_t flags = state.flags & HOST_SEEN;
    if (is_caps_word_on()) {
        flags |= CAPS_WORD_ON;
    }
    uint8_t oneshot_layer = is_oneshot_layer_active() ? get_oneshot_layer() : 0xff;
    if (oneshot_layer == U_CZ) {
        flags |= CZ_ARMED;
    }
    layer_state_t layers = layer_state | default_layer_state;
    if (layers & (((layer_state_t)1 << U_GAME) | ((layer_state_t)1 << U_GAMENUM) | ((layer_state_t)1 << U_GAMEFN))) {
        flags |= GAMING;
    }
    if (layers & ((layer_state_t)1 << U_GAMEFN)) {
        flags |= GAME_BANK;
    }
    uint8_t layout = get_highest_layer(default_layer_state);
    uint8_t layer = get_highest_layer(layers);
    uint8_t held_mods = get_mods();
    uint8_t oneshot_mods = get_oneshot_mods();
    uint8_t leds = host_keyboard_led_state().raw;
    if (state.layout != layout || state.layer != layer || state.held_mods != held_mods || state.oneshot_mods != oneshot_mods ||
        state.oneshot_layer != oneshot_layer || state.flags != flags || state.leds != leds) {
        state.layout = layout;
        state.layer = layer;
        state.held_mods = held_mods;
        state.oneshot_mods = oneshot_mods;
        state.oneshot_layer = oneshot_layer;
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

// Exactly five glyphs per row. Text never relies on driver wrapping or newlines.
// The last two rows belong to the 16x16 companion, not text.
static char displayed[14][5];
static uint16_t displayed_inverse;
static uint8_t displayed_sprite = 0xff;
static uint8_t previous_visual = 0xff;
static uint8_t previous_power = 0xff;

static void line_flash(char frame[14][5], uint8_t row, const char *text) {
    for (uint8_t col = 0; col < 5; ++col) {
        char c = pgm_read_byte(text + col);
        if (!c) {
            break;
        }
        frame[row][col] = c;
    }
}

#define LINE(row, text) line_flash(frame, row, PSTR(text))

static const char layer_names[][6] PROGMEM = {
    [U_BASE] = "Base", [U_EXTRA] = "CM-DH", [U_TAP] = "Tap", [U_BUTTON] = "Buttn",
    [U_NAV] = "Nav", [U_MOUSE] = "Mouse", [U_MEDIA] = "Media", [U_NUM] = "Num",
    [U_SYM] = "Sym", [U_FUN] = "Fun", [U_GAME] = "Game", [U_GAMENUM] = "G.Num",
    [U_CZ] = "CZ", [U_GAMEFN] = "Bank2"
};
static const char page_names[PAGE_COUNT][6] PROGMEM = { "Auto", "Hints", "Host", "Learn", "Pal" };

static void layout_line(char frame[14][5], uint8_t row) {
    if (state.layout == U_BASE || state.layout == U_TAP) {
        line_flash(frame, row, PSTR("QWRTY"));
    } else if (state.layout == U_EXTRA) {
        line_flash(frame, row, PSTR("CM-DH"));
    } else {
        line_flash(frame, row, layer_names[state.layout]);
    }
}

static void mods_line(char frame[14][5], uint8_t row, uint8_t mods) {
    frame[row][0] = mods & MOD_MASK_CTRL ? 'C' : '-';
    frame[row][1] = mods & MOD_MASK_SHIFT ? 'S' : '-';
    frame[row][2] = mods & MOD_BIT(KC_LALT) ? 'A' : '-';
    frame[row][3] = mods & MOD_MASK_GUI ? 'G' : '-';
    frame[row][4] = mods & MOD_BIT(KC_RALT) ? 'R' : '-';
}

static bool host_fresh(void) {
    return (state.flags & HOST_SEEN) && state.host_age < HOST_STALE_MS;
}

static void microphone_line(char frame[14][5], uint8_t row) {
    if (!host_fresh() || !(state.host[5] & MIC_KNOWN)) {
        line_flash(frame, row, PSTR("?"));
    } else {
        // OPEN describes the default source's mute state, never app recording.
        line_flash(frame, row, state.host[5] & MIC_MUTED ? PSTR("MUTE") : PSTR("OPEN"));
    }
}

static void dashboard(char frame[14][5]) {
    layout_line(frame, 0);
    LINE(1, "Layer");
    line_flash(frame, 2, layer_names[state.layer]);
    LINE(3, "Held");
    mods_line(frame, 4, state.held_mods);
    LINE(5, "1shot");
    mods_line(frame, 6, state.oneshot_mods);
    line_flash(frame, 7, state.flags & CZ_ARMED ? PSTR("CZ +") : PSTR("CZ -"));
    line_flash(frame, 8, state.flags & CAPS_WORD_ON ? PSTR("CW ON") : PSTR("CW --"));
    led_t leds = {.raw = state.leds};
    frame[9][0] = leds.num_lock ? 'N' : '-';
    frame[9][2] = leds.caps_lock ? 'C' : '-';
    frame[9][4] = leds.scroll_lock ? 'S' : '-';
    if (state.flags & GAMING) {
        line_flash(frame, 10, state.flags & GAME_BANK ? PSTR("17-20") : PSTR("13-16"));
    } else {
        LINE(10, "Type");
    }
    line_flash(frame, 11, page_names[state.page]);
    LINE(12, "Mic");
    microphone_line(frame, 13);
}

// Compact, static reminders of the selected VI-navigation Miryoku maps.
// Rows are positions/actions, not an alternative copy of the keymap enum.
static const char hints[][12][6] PROGMEM = {
    [U_BASE] = {"V+M", "CZ x1", "C+,", "QW/CM", "C+M", "CapsW", "X+. >", "Page", "Home", "hold", "GACS-", "-SCAG"},
    [U_EXTRA] = {"V+M", "CZ x1", "C+,", "QW/CM", "C+M", "CapsW", "X+. >", "Page", "Home", "hold", "GACS-", "-SCAG"},
    [U_TAP] = {"V+M", "CZ x1", "C+,", "QW/CM", "C+M", "CapsW", "X+. >", "Page", "Plain", "keys", "No", "mods"},
    [U_BUTTON] = {"L>R", "Undo", "Cut", "Copy", "Paste", "Redo", "R>L", "same", "Thumb", "3 1 2", "2 1 3", "L / R"},
    [U_NAV] = {"Right", "<v^>W", "W: CW", "Below", "Home", "PgDn", "PgUp", "End", "Thumb", "Enter", "Bksp", "Del"},
    [U_MOUSE] = {"Right", "<v^>", "Below", "Wheel", "<v^>", "Thumb", "Btn2", "Btn1", "Btn3", "", "", ""},
    [U_MEDIA] = {"Right", "Prev", "Vol-", "Vol+", "Next", "Thumb", "Stop", "Play", "Mute", "Top", "RGB", ""},
    [U_NUM] = {"Left", "[789]", ";456=", "`123\\", "Thumb", ". 0 -", "Right", "mods", "S C A", "G", "", ""},
    [U_SYM] = {"Left", "{&*(}", ":$%^+", "~!@#|", "Thumb", "( ) _", "Right", "mods", "S C A", "G", "", ""},
    [U_FUN] = {"Left", "F-key", "12 7", "8 9", "11 4", "5 6", "10 1", "2 3", "Outer", "PrScr", "ScrLk", "Pause"},
    [U_GAME] = {"Outer", "L F13", "L F14", "L F15", "R F16", "Bank", "R low", "hold", "17-20", "Thumb", "Num", "RtopQ"},
    [U_GAMENUM] = {"Left", "E123T", "S456G", "C789B", "Right", "F7-9", "F4-6", "F1-3", "Thumb", "- 0 .", "L Alt", "QWRTY"},
    [U_CZ] = {"QWpos", "W e^", "E e'", "U uo", "J u'", "Y y'", "C c^", "N n^", "R r^", "S s^", "Z z^", "1 key"},
    [U_GAMEFN] = {"Bank2", "Outer", "L F17", "L F18", "L F19", "R F20", "Hold", "R low", "Inner", "game", "keys", "stay"}
};

static void hints_page(char frame[14][5]) {
    LINE(0, "Hints");
    line_flash(frame, 1, layer_names[state.layer]);
    for (uint8_t row = 0; row < 12; ++row) {
        line_flash(frame, row + 2, hints[state.layer][row]);
    }
}

static void host_page(char frame[14][5]) {
    LINE(0, "Host");
    bool fresh = host_fresh();
    line_flash(frame, 1, fresh ? PSTR("Live") : state.flags & HOST_SEEN ? PSTR("Stale") : PSTR("Wait"));
    LINE(2, "Mic");
    microphone_line(frame, 3);
    LINE(4, "Sound");
    uint8_t flags = fresh ? state.host[5] : 0;
    if (!(flags & OUTPUT_KNOWN)) {
        LINE(5, "?");
    } else if (flags & OUTPUT_MUTED) {
        LINE(5, "MUTE");
    } else {
        uint8_t volume = state.host[6];
        frame[5][0] = volume == 100 ? '1' : ' ';
        frame[5][1] = volume >= 10 ? '0' + volume / 10 % 10 : ' ';
        frame[5][2] = '0' + volume % 10;
        frame[5][3] = '%';
    }
    LINE(6, "Desk");
    if (flags & WORKSPACE_KNOWN) {
        for (uint8_t i = 0; i < 8 && state.host[8 + i]; ++i) {
            frame[7 + i / 5][i % 5] = state.host[8 + i];
        }
    } else {
        LINE(7, "?");
    }
    if (flags & MEDIA_KNOWN) {
        line_flash(frame, 9, state.host[7] == 2 ? PSTR("PLAY") : state.host[7] == 1 ? PSTR("PAUSE") : PSTR("STOP"));
        if (!state.host[16]) {
            LINE(10, "None");
        }
        for (uint8_t i = 0; i < 16 && state.host[16 + i]; ++i) {
            frame[10 + i / 5][i % 5] = state.host[16 + i];
        }
    } else {
        LINE(9, "Media");
        LINE(10, "?");
    }
}

static void practice_page(char frame[14][5]) {
    LINE(0, "Learn");
    bool colemak = state.layout == U_EXTRA;
    if (state.layout != U_BASE && state.layout != U_EXTRA && state.layout != U_TAP) {
        LINE(1, "Type");
        LINE(3, "Enter");
        LINE(4, "QWRTY");
        LINE(5, "or");
        LINE(6, "CM-DH");
        LINE(8, "first");
        return;
    }
    layout_line(frame, 1);
    LINE(2, "Left");
    line_flash(frame, 3, colemak ? PSTR("QWFPB") : PSTR("QWERT"));
    line_flash(frame, 4, colemak ? PSTR("ARSTG") : PSTR("ASDFG"));
    line_flash(frame, 5, colemak ? PSTR("ZXCDV") : PSTR("ZXCVB"));
    LINE(6, "Right");
    line_flash(frame, 7, colemak ? PSTR("JLUY'") : PSTR("YUIOP"));
    line_flash(frame, 8, colemak ? PSTR("MNEIO") : PSTR("HJKL'"));
    line_flash(frame, 9, colemak ? PSTR("KH,./") : PSTR("NM,./"));
    LINE(10, "Home");
    if (state.layout == U_TAP) {
        LINE(11, "Plain");
        LINE(12, "keys");
    } else {
        LINE(11, "GACS-");
        LINE(12, "-SCAG");
        LINE(13, "Hold");
    }
}

static void companion_page(char frame[14][5]) {
    LINE(0, "Pal");
    layout_line(frame, 1);
    if (state.key_age < REACTION_MS) {
        LINE(4, "Hi!");
        LINE(6, "Nice");
        LINE(7, "keys.");
    } else {
        LINE(4, "Rest");
        LINE(6, "Tap a");
        LINE(7, "key.");
    }
    LINE(9, "I nap");
    LINE(10, "at30s");
    LINE(12, "X+. >");
    LINE(13, "Auto");
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

static void draw_frame(char frame[14][5], uint16_t inverse, uint8_t sprite) {
    for (uint8_t row = 0; row < 14; ++row) {
        uint16_t bit = (uint16_t)1 << row;
        if (memcmp(displayed[row], frame[row], 5) != 0 || ((displayed_inverse ^ inverse) & bit)) {
            oled_set_cursor(0, row);
            for (uint8_t col = 0; col < 5; ++col) {
                oled_write_char(frame[row][col], inverse & bit);
            }
            memcpy(displayed[row], frame[row], 5);
        }
    }
    displayed_inverse = inverse;
    if (sprite != displayed_sprite) {
        for (uint8_t page = 0; page < 2; ++page) {
            for (uint8_t col = 0; col < 32; ++col) {
                uint8_t pixels = col >= 8 && col < 24 ? pgm_read_byte(&companion[sprite][page * 16 + col - 8]) : 0;
                oled_write_raw_byte(pixels, (14 + page) * 32 + col);
            }
        }
        displayed_sprite = sprite;
    }
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
    char frame[14][5];
    memset(frame, ' ', sizeof(frame));
    uint16_t inverse = 1;
    if (is_keyboard_master()) {
        dashboard(frame);
        if (state.flags & CZ_ARMED) {
            inverse |= (uint16_t)1 << 7;
        }
    } else if (link_lost) {
        LINE(0, "Link");
        line_flash(frame, 1, link_seen ? PSTR("Lost") : PSTR("Wait"));
        LINE(4, "Check");
        LINE(5, "split");
        LINE(6, "cable");
        LINE(9, "State");
        LINE(10, "not");
        LINE(11, "live");
    } else {
        uint8_t page = state.page;
        if (page == PAGE_AUTO) {
            page = state.layer == U_BASE || state.layer == U_EXTRA || state.layer == U_TAP ? PAGE_HOST : PAGE_HINTS;
        }
        switch (page) {
            case PAGE_HINTS: hints_page(frame); break;
            case PAGE_HOST: host_page(frame); break;
            case PAGE_PRACTICE: practice_page(frame); break;
            case PAGE_COMPANION: companion_page(frame); break;
            default: break;
        }
    }
    draw_frame(frame, inverse, sprite);
    return false;
}
