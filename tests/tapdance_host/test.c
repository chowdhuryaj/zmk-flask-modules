/*
 * Host check for src/flask_tapdance.c (no Zephyr toolchain needed): the
 * engine source is compiled unmodified against stub/ and driven with
 * press/release/timer/interrupt/keycode events. Outputs are logged as
 * "+K"/"-K" (usage K) and "+b11:P1"/"-b11:P1" (&fak). Run: make
 *
 * SPDX-License-Identifier: MIT
 */
#include "../../src/flask_tapdance.c"

#include <assert.h>
#include <stdio.h>

#define ID_FTD 10
#define ID_FAK 11
#define OTHER_POS 9
#define OTHER_KC 99

int64_t stub_now = 1000;
static char out_log[512];

static void log_out(const char *fmt, uint32_t v) {
    size_t n = strlen(out_log);

    snprintf(out_log + n, sizeof(out_log) - n, fmt, v);
}

void raise_zmk_keycode_state_changed_from_encoded(uint32_t kc, bool pressed, int64_t ts) {
    struct zmk_keycode_state_changed ev = {{STUB_EV_KEYCODE}, kc, pressed, ts};

    log_out(pressed ? "+%u " : "-%u ", kc);
    stub_listener_flask_tapdance(&ev.header); /* real ZMK: tapdance hears its own outputs too */
}

zmk_behavior_local_id_t zmk_behavior_get_local_id(const char *name) {
    if (strcmp(name, "zmk_behavior_flask_tapdance") == 0) {
        return ID_FTD;
    }
    return strcmp(name, "zmk_behavior_flask_adaptive") == 0 ? ID_FAK : UINT16_MAX;
}

const char *zmk_behavior_find_behavior_name_from_local_id(zmk_behavior_local_id_t id) {
    return id == ID_FAK ? "fak" : NULL;
}

int zmk_behavior_invoke_binding(const struct zmk_behavior_binding *b,
                                struct zmk_behavior_binding_event ev, bool pressed) {
    log_out(pressed ? "+b11:%u " : "-b11:%u ", b->param1);
    return 0;
}

static char saved_key[32];
static size_t saved_len;
static uint8_t saved_buf[1024];

int settings_save_one(const char *name, const void *value, size_t len) {
    snprintf(saved_key, sizeof(saved_key), "%s", name);
    saved_len = len;
    memcpy(saved_buf, value, len);
    return 0;
}

int settings_delete(const char *name) { return 0; }

static ssize_t read_buf(void *arg, void *data, size_t len) {
    memcpy(data, arg, len);
    return (ssize_t)len;
}

/* --- drivers --- */

static void advance(int ms) {
    while (ms-- > 0) {
        stub_now++;
        for (int i = 0; i < TD_MAX_HELD; i++) {
            struct k_work_delayable *w = &dances[i].release_timer;

            if (w->pending && w->due <= stub_now) {
                w->pending = false;
                w->work.handler(&w->work);
            }
        }
    }
}

static void position(uint32_t pos, bool state) {
    struct zmk_position_state_changed ev = {{STUB_EV_POSITION}, pos, state, stub_now};

    stub_listener_flask_tapdance(&ev.header);
}

/* The dance for slot N sits at key position N. */
static void td_down(uint8_t slot) {
    position(slot, true);
    flask_tapdance_pressed(slot, slot, stub_now);
}

static void td_up(uint8_t slot) {
    position(slot, false);
    flask_tapdance_released(slot, slot, stub_now);
}

static void other_down(void) {
    position(OTHER_POS, true);
    raise_zmk_keycode_state_changed_from_encoded(OTHER_KC, true, stub_now);
}

static void other_up(void) {
    position(OTHER_POS, false);
    raise_zmk_keycode_state_changed_from_encoded(OTHER_KC, false, stub_now);
}

static void set_out(uint8_t slot, uint8_t tap, bool hold, uint8_t action, uint16_t bid,
                    uint32_t p1) {
    struct flask_tapdance_output o = {.action = action, .behavior_id = bid, .param1 = p1};

    assert(flask_tapdance_output_set(slot, tap, hold, &o) == 0);
}

static int fails;

static void check(const char *name, bool ok, const char *got) {
    printf("%s %-36s %s\n", ok ? "ok  " : "FAIL", name, got);
    fails += !ok;
}

static void expect(const char *name, const char *want) {
    check(name, strcmp(out_log, want) == 0, out_log);
    out_log[0] = 0;
}

static void idle(void) {
    advance(1000);
    out_log[0] = 0;
}

int main(void) {
    struct flask_tapdance_slot s;

    sys_init_flask_tapdance_init();
    assert(sizeof(struct flask_tapdance_slot) == 4 + 2 * FLASK_TAPDANCE_TAPS * 11);

    /* slot 0: taps 4,5,6; holds on counts 1,2 (20, 21); count 3 none = fast path */
    set_out(0, 0, false, FLASK_TD_OUT_USAGE, 0, 4);
    set_out(0, 1, false, FLASK_TD_OUT_USAGE, 0, 5);
    set_out(0, 2, false, FLASK_TD_OUT_USAGE, 0, 6);
    set_out(0, 0, true, FLASK_TD_OUT_USAGE, 0, 20);
    set_out(0, 1, true, FLASK_TD_OUT_USAGE, 0, 21);
    /* slot 1: taps 7,8; hold 24 on the LAST count */
    set_out(1, 0, false, FLASK_TD_OUT_USAGE, 0, 7);
    set_out(1, 1, false, FLASK_TD_OUT_USAGE, 0, 8);
    set_out(1, 1, true, FLASK_TD_OUT_USAGE, 0, 24);
    /* slot 2: taps 9,10, prior idle 150 */
    set_out(2, 0, false, FLASK_TD_OUT_USAGE, 0, 9);
    set_out(2, 1, false, FLASK_TD_OUT_USAGE, 0, 10);
    flask_tapdance_prior_idle_set(2, 150);

    td_down(0); advance(50); td_up(0); advance(149);
    expect("single tap waits the term", "");
    advance(1);
    expect("single tap", "+4 -4 ");

    idle();
    td_down(0); advance(50); td_up(0); advance(50); td_down(0); advance(50); td_up(0);
    advance(300);
    expect("double tap", "+5 -5 ");

    idle();
    td_down(0); advance(199);
    expect("tap-hold before term", "");
    advance(1);
    expect("tap-hold fires at term", "+20 ");
    advance(500); td_up(0);
    expect("tap-hold released with key", "-20 ");

    idle();
    td_down(0); advance(50); td_up(0); advance(50); td_down(0); advance(200);
    expect("double-hold", "+21 ");
    td_up(0);
    expect("double-hold release", "-21 ");

    idle();
    td_down(0); advance(50); other_down(); advance(400); other_up(); td_up(0);
    expect("interrupt = tap, not hold", "+4 +99 -99 -4 ");

    idle();
    td_down(0); td_up(0); td_down(0); td_up(0); td_down(0);
    expect("max count w/o hold fires at press", "+6 ");
    td_up(0);
    expect("max count release", "-6 ");

    idle();
    td_down(1); advance(30); td_up(1); advance(30); td_down(1); advance(100);
    expect("last count w/ hold waits", "");
    td_up(1);
    expect("last count w/ hold, release = tap", "+8 -8 ");
    idle();
    td_down(1); td_up(1); td_down(1); advance(200);
    expect("last count w/ hold, term = hold", "+24 ");
    td_up(1);
    expect("last count hold release", "-24 ");

    idle();
    other_down(); other_up(); advance(100); td_down(2);
    expect("bypass: tap 1 at press", "+99 -99 +9 ");
    advance(500); td_up(2);
    expect("bypass: held until release", "-9 ");
    idle();
    other_down(); other_up(); advance(150); td_down(2);
    expect("no bypass at prior_idle ms", "+99 -99 ");
    td_up(2); advance(50); td_down(2);
    expect("no bypass mid-dance (max count)", "+10 ");
    td_up(2);
    idle();
    other_down(); other_up(); advance(10); td_down(0);
    expect("prior_idle 0 = bypass off", "+99 -99 ");
    td_up(0);

    idle();
    set_out(3, 0, false, FLASK_TD_OUT_BEHAVIOR, ID_FTD, 1);
    set_out(3, 0, true, FLASK_TD_OUT_BEHAVIOR, ID_FTD, 1);
    flask_tapdance_slot_get(3, &s);
    check("&ftd output refused (tap + hold)",
          s.taps[0].action == FLASK_TD_OUT_NONE && s.holds[0].action == FLASK_TD_OUT_NONE, "");
    set_out(3, 0, false, FLASK_TD_OUT_BEHAVIOR, ID_FAK, 2);
    flask_tapdance_slot_get(3, &s);
    check("&fak output kept", s.taps[0].action == FLASK_TD_OUT_BEHAVIOR &&
                                  s.taps[0].behavior_id == ID_FAK, "");
    td_down(3); td_up(3);
    expect("&fak output fires", "+b11:2 -b11:2 ");

    /* v1 blob: term u16 + taps[N] (11 bytes each), no holds / prior idle */
    uint8_t v1[2 + FLASK_TAPDANCE_TAPS * 11] = {0};
    struct flask_tapdance_output o = {.action = FLASK_TD_OUT_USAGE, .param1 = 4};
    uint16_t term = 300;
    char got[64];

    memcpy(v1, &term, 2);
    memcpy(v1 + 2, &o, sizeof(o));
    assert(flask_tapdance_settings_restore("s5", sizeof(v1), read_buf, v1) == 0);
    flask_tapdance_slot_get(5, &s);
    bool mig = s.term_ms == 300 && s.prior_idle_ms == 0 && s.taps[0].param1 == 4 &&
               s.taps[1].action == FLASK_TD_OUT_NONE && s.holds[0].action == FLASK_TD_OUT_NONE &&
               (slots_dirty & BIT64(5));
    slots_dirty = BIT64(5); /* save only what the migration marked */
    flask_tapdance_save();
    mig = mig && strcmp(saved_key, "flask/tapdance/s5") == 0 && saved_len == sizeof(s);
    snprintf(got, sizeof(got), "%zu -> %zu bytes", sizeof(v1), saved_len);
    check("v1 blob migrates, SAVE rewrites it", mig, got);
    assert(flask_tapdance_settings_restore("s6", saved_len, read_buf, saved_buf) == 0);
    flask_tapdance_slot_get(6, &s);
    check("new blob round-trips clean", s.term_ms == 300 && !(slots_dirty & BIT64(6)), "");
    check("bad blob size ignored",
          flask_tapdance_settings_restore("s7", 7, read_buf, saved_buf) == 0 &&
              (flask_tapdance_slot_get(7, &s), s.term_ms == 0),
          "");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
