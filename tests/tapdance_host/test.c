/*
 * Host check for src/flask_tapdance.c (no Zephyr toolchain needed): the
 * engine source is compiled unmodified against stub/ and driven with
 * press/release/timer/interrupt/keycode events. Outputs are logged as
 * "+K"/"-K" (usage K) and "+b11:P1"/"-b11:P1" (&fak). Run: make
 *
 * SPDX-License-Identifier: MIT
 */
#include "../../src/flask_tapdance.c"
#include "../../src/flask_tapdance_replay.c"

#include <assert.h>
#include <stdio.h>

#define ID_FTD 10
#define ID_FAK 11
#define ID_SDEL 12
#define ID_WDEL 13
#define ID_LATE 14

static bool late_ok;
static int deletes;
#define OTHER_POS 9
#define OTHER_KC 99

int64_t stub_now = 1000;
static char out_log[512];
static char full_log[16384]; /* never cleared: the balance check at the end reads it */

static void log_out(const char *fmt, uint32_t v) {
    size_t n = strlen(out_log);

    snprintf(out_log + n, sizeof(out_log) - n, fmt, v);
    n = strlen(full_log);
    snprintf(full_log + n, sizeof(full_log) - n, fmt, v);
}

void raise_zmk_keycode_state_changed_from_encoded(uint32_t kc, bool pressed, int64_t ts) {
    struct zmk_keycode_state_changed ev = {{STUB_EV_KEYCODE}, (uint16_t)(kc >> 16), kc & 0xFFFF, pressed, ts};

    log_out(pressed ? "+%u " : "-%u ", kc);
    stub_listener_flask_tapdance(&ev.header); /* real ZMK: tapdance hears its own outputs too */
}

zmk_behavior_local_id_t zmk_behavior_get_local_id(const char *name) {
    if (strcmp(name, "zmk_behavior_flask_tapdance") == 0) {
        return ID_FTD;
    }
    if (strcmp(name, "sdel_td") == 0) {
        return ID_SDEL; /* fixed-slot dance instance (stub devicetree) */
    }
    if (strcmp(name, "wdel_mm") == 0) {
        return ID_WDEL;
    }
    if (strcmp(name, "late") == 0) {
        return late_ok ? ID_LATE : 0; /* local id not assigned yet */
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

int settings_delete(const char *name) {
    deletes++;
    return 0;
}

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
    assert(sizeof(struct flask_tapdance_slot) == 7 + 2 * FLASK_TAPDANCE_TAPS * 11);

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
    expect("max count after no-bypass start", "+10 ");
    td_up(2);
    idle();
    td_down(2); td_up(2);
    raise_zmk_keycode_state_changed_from_encoded(99, true, stub_now);
    raise_zmk_keycode_state_changed_from_encoded(99, false, stub_now);
    advance(50); td_down(2);
    expect("no bypass mid-dance", "+99 -99 +10 ");
    td_up(2);
    idle();
    raise_zmk_keycode_state_changed_from_encoded(0x000700E0, true, stub_now);
    advance(50); td_down(2);
    expect("held modifier does not bypass", "+458976 ");
    td_up(2); advance(50); td_down(2);
    expect("dance continues under a held mod", "+10 ");
    td_up(2);
    raise_zmk_keycode_state_changed_from_encoded(0x000700E0, false, stub_now);
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

    /* ---- extra paths ---- */
    /* slot helpers: len-1 slots 8,12..15 (taps 50,52..55), 2-tap slots 10,11 (30,31 / 40,41) */
    const uint8_t one[] = {8, 12, 13, 14, 15};
    int rc;

    for (int i = 0; i < 5; i++) {
        set_out(one[i], 0, false, FLASK_TD_OUT_USAGE, 0, one[i] == 8 ? 50 : 40 + one[i]);
    }
    set_out(10, 0, false, FLASK_TD_OUT_USAGE, 0, 30);
    set_out(10, 1, false, FLASK_TD_OUT_USAGE, 0, 31);
    set_out(11, 0, false, FLASK_TD_OUT_USAGE, 0, 40);
    set_out(11, 1, false, FLASK_TD_OUT_USAGE, 0, 41);

    idle();
    td_down(1); td_up(1); td_down(1); advance(100);
    other_down();
    expect("at_max interrupt = tap, key down", "+8 +99 ");
    advance(1000);
    expect("at_max interrupt: timer stopped", "");
    td_up(1); other_up();
    expect("at_max interrupt: released once", "-8 -99 ");

    idle();
    other_down(); other_up(); advance(100); td_down(2);
    expect("bypass held", "+99 -99 +9 ");
    other_down();
    expect("interrupt of held bypass: no refire", "+99 ");
    advance(1000);
    td_up(2);
    expect("bypass release once", "-9 ");
    other_up();
    expect("bypass interrupt other release", "-99 ");

    idle();
    td_down(0); td_up(0);
    other_down();
    expect("interrupt after dance key released", "+4 -4 +99 ");
    other_up();
    advance(1000);
    expect("no late refire", "-99 ");

    idle();
    td_down(10);
    td_down(11);
    expect("two dances: 2nd press resolves 1st", "+30 ");
    td_up(10);
    expect("two dances: 1st release", "-30 ");
    advance(200);
    expect("two dances: 2nd timer = tap", "+40 ");
    td_up(11);
    expect("two dances: 2nd release", "-40 ");
    idle();
    td_down(10); td_up(10); td_down(11); td_up(11);
    expect("two dances: 1st resolved on 2nd", "+30 -30 ");
    advance(200);
    expect("two dances: 2nd times out", "+40 -40 ");

    idle();
    for (int i = 0; i < 5; i++) {
        position(one[i], true);
        rc = flask_tapdance_pressed(one[i], one[i], stub_now);
        check("TD_MAX_HELD slot rc", i < TD_MAX_HELD ? rc == 0 : rc == -ENOMEM, "");
    }
    expect("5th simultaneous dance dropped", "+50 +52 +53 +54 ");
    for (int i = 0; i < 5; i++) {
        position(one[i], false);
        flask_tapdance_released(one[i], one[i], stub_now);
    }
    expect("releases mirror presses, 5th none", "-50 -52 -53 -54 ");
    td_down(15); td_up(15);
    expect("5th key works once a slot frees", "+55 -55 ");

    /* rewrite mid-dance */
    idle();
    td_down(13);
    set_out(13, 0, false, FLASK_TD_OUT_NONE, 0, 0);
    td_up(13);
    expect("tap removed while held", "+53 -53 ");
    set_out(13, 0, false, FLASK_TD_OUT_USAGE, 0, 53);
    td_down(13);
    set_out(13, 0, false, FLASK_TD_OUT_USAGE, 0, 77);
    td_up(13);
    expect("tap changed while held", "+53 -53 ");
    set_out(13, 0, false, FLASK_TD_OUT_USAGE, 0, 53);

    idle();
    td_down(0); advance(200);
    expect("hold fired", "+20 ");
    set_out(0, 0, true, FLASK_TD_OUT_USAGE, 0, 25);
    advance(300); td_up(0);
    expect("hold changed while held, old released", "-20 ");
    td_down(0); advance(200);
    expect("changed hold used next time", "+25 ");
    set_out(0, 0, true, FLASK_TD_OUT_NONE, 0, 0);
    td_up(0);
    expect("hold removed while held", "-25 ");
    td_down(0); advance(200);
    expect("removed hold falls back to tap", "+4 ");
    td_up(0);
    expect("removed hold release", "-4 ");
    set_out(0, 0, true, FLASK_TD_OUT_USAGE, 0, 20);

    idle();
    td_down(1);
    set_out(1, 0, false, FLASK_TD_OUT_NONE, 0, 0);
    set_out(1, 1, false, FLASK_TD_OUT_NONE, 0, 0);
    set_out(1, 1, true, FLASK_TD_OUT_NONE, 0, 0);
    advance(200);
    td_up(1);
    expect("slot emptied mid-dance: nothing, no crash", "");
    td_down(1); td_up(1); advance(300);
    expect("emptied slot stays inert", "");
    set_out(1, 0, false, FLASK_TD_OUT_USAGE, 0, 7);
    set_out(1, 1, false, FLASK_TD_OUT_USAGE, 0, 8);
    set_out(1, 1, true, FLASK_TD_OUT_USAGE, 0, 24);
    td_down(1); td_up(1); advance(200);
    expect("slot refilled, dance fresh", "+7 -7 ");

    idle();
    td_down(1); td_up(1); td_down(1);
    set_out(1, 1, false, FLASK_TD_OUT_NONE, 0, 0); /* len shrinks 2 -> 1 under count 2 */
    set_out(1, 1, true, FLASK_TD_OUT_NONE, 0, 0);
    td_up(1);
    expect("len shrinks mid-dance: clamps to tap 1", "+7 -7 ");
    set_out(1, 1, false, FLASK_TD_OUT_USAGE, 0, 8);
    set_out(1, 1, true, FLASK_TD_OUT_USAGE, 0, 24);

    /* term / prior idle clamps */
    idle();
    flask_tapdance_term_set(10, 10);
    flask_tapdance_slot_get(10, &s);
    check("term 10 clamps to 50", s.term_ms == 50, "");
    td_down(10); td_up(10); advance(49);
    expect("term 50: not yet at 49", "");
    advance(1);
    expect("term 50: resolves at 50", "+30 -30 ");
    flask_tapdance_term_set(11, 5000);
    flask_tapdance_slot_get(11, &s);
    check("term 5000 clamps to 1000", s.term_ms == 1000, "");
    td_down(11); td_up(11); advance(999);
    expect("term 1000: not yet at 999", "");
    advance(1);
    expect("term 1000: resolves at 1000", "+40 -40 ");
    flask_tapdance_term_set(11, 0);
    flask_tapdance_slot_get(11, &s);
    check("term 0 stored as 0 (default)", s.term_ms == 0, "");
    td_down(11); td_up(11); advance(199);
    expect("term 0: nothing at 199", "");
    advance(1);
    expect("term 0: default 200", "+40 -40 ");
    flask_tapdance_term_set(10, 0);
    flask_tapdance_prior_idle_set(2, 5000);
    flask_tapdance_slot_get(2, &s);
    check("prior idle 5000 clamps to 1000", s.prior_idle_ms == 1000, "");
    idle();
    other_down(); other_up(); advance(999); td_down(2);
    expect("prior idle 1000: bypass at 999", "+99 -99 +9 ");
    td_up(2);
    expect("prior idle 1000: release", "-9 ");
    flask_tapdance_prior_idle_set(2, 150);

    /* quick re-press after resolution starts fresh */
    idle();
    td_down(0); td_up(0); advance(200);
    expect("repress: first single tap", "+4 -4 ");
    td_down(0); td_up(0); advance(199);
    expect("repress: waits fresh term", "");
    advance(1);
    expect("repress: single again, not double", "+4 -4 ");
    td_down(0); advance(200); td_up(0);
    expect("repress: hold", "+20 -20 ");
    td_down(0); advance(200);
    expect("repress after hold: hold again, not count 2", "+20 ");
    td_up(0);
    expect("repress hold release", "-20 ");
    td_down(8); td_up(8); td_down(8); td_up(8);
    expect("repress after max-fast: fresh", "+50 -50 +50 -50 ");

    /* flask_holdtap replay: RAISE_AT(holdtap) skips flask_tapdance.c's
     * listener; the replayed P starts a dance via the keymap, the replayed X
     * reaches only the replay listener (linked after holdtap). */
    idle();
    {
        struct zmk_position_state_changed x = {{STUB_EV_POSITION}, OTHER_POS, true, stub_now};

        flask_tapdance_pressed(0, 0, stub_now);
        stub_listener_flask_tapdance_replay(&x.header);
        raise_zmk_keycode_state_changed_from_encoded(OTHER_KC, true, stub_now);
        expect("replayed press interrupts the dance", "+4 +99 ");
        stub_listener_flask_tapdance_replay(&x.header); /* idempotent */
        advance(300);
        expect("replay interrupt idempotent", "");
        other_up();
        td_up(0);
        expect("replayed dance releases", "-99 -4 ");
    }

    /* Completed flask_combo: its keys never reach the listener; combos calls
     * interrupt(UINT32_MAX) before typing (the dance key may be a combo key). */
    idle();
    td_down(0); td_up(0); advance(50);
    flask_tapdance_interrupt(UINT32_MAX, stub_now);
    raise_zmk_keycode_state_changed_from_encoded(77, true, stub_now);
    expect("combo interrupts the dance first", "+4 -4 +77 ");
    raise_zmk_keycode_state_changed_from_encoded(77, false, stub_now);
    td_down(0); advance(50);
    flask_tapdance_interrupt(UINT32_MAX, stub_now);
    expect("combo interrupts a held dance too", "-77 +4 ");
    td_up(0);
    expect("combo-interrupted held dance release", "-4 ");

    /* The dance's own delayed output stamps its physical press time, not the
     * output's event time (prior idle 150 on slot 2). */
    idle();
    td_down(2); td_up(2); advance(200);
    expect("timer-resolved output", "+9 -9 ");
    advance(50); td_down(2); /* 250 ms after the physical press */
    expect("timer output does not arm bypass", "");
    td_up(2); advance(300);
    expect("timer output: next dance taps", "+9 -9 ");
    idle();
    td_down(2); td_up(2); advance(100);
    position(OTHER_POS, true); /* non-keycode key (layer) interrupts */
    expect("interrupt-resolved output", "+9 -9 ");
    position(OTHER_POS, false);
    advance(100); td_down(2); /* 200 ms after the physical press */
    expect("interrupt output does not arm bypass", "");
    td_up(2); advance(300);
    out_log[0] = 0;
    idle();
    td_down(2); td_up(2); advance(50);
    position(OTHER_POS, true);
    position(OTHER_POS, false);
    advance(50); td_down(2); /* 100 ms after the physical press */
    expect("own output still stamps the press time", "+9 -9 +9 ");
    td_up(2);
    expect("own output stamp: release", "-9 ");

    /* After-release window (slot 0: taps 4,5,6, holds 20,21, term 200). */
    idle();
    td_down(0); advance(20); td_up(0); advance(179);
    expect("window 0: waits the press term", "");
    advance(1);
    expect("window 0: resolves at press + term", "+4 -4 ");
    flask_tapdance_release_set(0, 80);
    idle();
    td_down(0); advance(20); td_up(0); advance(79);
    expect("window: waits release + 80", "");
    advance(1);
    expect("window: resolves at release + 80", "+4 -4 ");
    idle();
    td_down(0); advance(150); td_up(0); advance(49);
    expect("window: press deadline kept if earlier", "");
    advance(1);
    expect("window: resolves at press + term", "+4 -4 ");
    idle();
    td_down(0); advance(20); td_up(0); advance(60); td_down(0); advance(20); td_up(0);
    advance(79);
    expect("window: double tap inside it counts", "");
    advance(1);
    expect("window: double tap at release + 80", "+5 -5 ");
    idle();
    td_down(0); advance(199);
    expect("window: tap-hold before term", "");
    advance(1);
    expect("window: tap-hold still holds", "+20 ");
    advance(100); td_up(0);
    expect("window: hold release", "-20 ");
    flask_tapdance_release_set(0, 5000);
    flask_tapdance_slot_get(0, &s);
    check("window clamps to 1000", s.release_ms == 1000, "");
    flask_tapdance_release_set(0, 0);

    /* v2 blob (v22): term + prior idle + taps + holds, no release_ms */
    {
        uint8_t v2[4 + 2 * FLASK_TAPDANCE_TAPS * 11] = {0};
        struct flask_tapdance_output h = {.action = FLASK_TD_OUT_USAGE, .param1 = 20};
        uint16_t v2_term = 250, v2_idle = 100;
        char v2_got[64];

        memcpy(v2, &v2_term, 2);
        memcpy(v2 + 2, &v2_idle, 2);
        memcpy(v2 + 4, &o, sizeof(o));
        memcpy(v2 + 4 + FLASK_TAPDANCE_TAPS * 11, &h, sizeof(h));
        assert(flask_tapdance_settings_restore("s9", sizeof(v2), read_buf, v2) == 0);
        flask_tapdance_slot_get(9, &s);
        bool up = s.term_ms == 250 && s.prior_idle_ms == 100 && s.release_ms == 0 &&
                  s.taps[0].param1 == 4 && s.holds[0].param1 == 20 && (slots_dirty & BIT64(9));
        slots_dirty = BIT64(9);
        flask_tapdance_save();
        up = up && strcmp(saved_key, "flask/tapdance/s9") == 0 && saved_len == sizeof(s);
        snprintf(v2_got, sizeof(v2_got), "%zu -> %zu bytes", sizeof(v2), saved_len);
        check("v2 blob upgrades, SAVE rewrites it", up, v2_got);
    }

    /* Eager (Super Delete feel), slot 4: taps 60,61,62, a hold 63 that must
     * be ignored, term 400 press-to-press. */
    set_out(4, 0, false, FLASK_TD_OUT_USAGE, 0, 60);
    set_out(4, 1, false, FLASK_TD_OUT_USAGE, 0, 61);
    set_out(4, 2, false, FLASK_TD_OUT_USAGE, 0, 62);
    set_out(4, 0, true, FLASK_TD_OUT_USAGE, 0, 63);
    flask_tapdance_term_set(4, 400);
    flask_tapdance_flags_set(4, 0xFF);
    flask_tapdance_slot_get(4, &s);
    check("eager: unknown flag bits cleared", s.flags == FLASK_TD_FLAG_EAGER, "");
    idle();
    td_down(4);
    expect("eager: tap 1 at press", "+60 ");
    td_up(4);
    expect("eager: tap 1 release", "-60 ");
    advance(100); td_down(4);
    expect("eager: tap 2 at press", "+61 ");
    td_up(4); advance(100); td_down(4);
    expect("eager: tap 3 at press", "-61 +62 ");
    td_up(4); advance(100); td_down(4); td_up(4);
    expect("eager: tap 4 sticks at the last", "-62 +62 -62 ");
    advance(399); td_down(4); td_up(4);
    expect("eager: inside term keeps going", "+62 -62 ");
    advance(401); td_down(4);
    expect("eager: pause > term restarts", "+60 ");
    td_up(4);
    out_log[0] = 0;
    advance(50); other_down(); other_up(); advance(50); td_down(4); td_up(4);
    expect("eager: interrupt ends the chain", "+99 -99 +60 -60 ");
    advance(50); td_down(4); advance(50); other_down(); other_up(); td_up(4);
    expect("eager: interrupt while held keeps it held", "+61 +99 -99 -61 ");
    advance(50); td_down(4);
    expect("eager: chain closed by interrupt restarts", "+60 ");
    advance(1000);
    expect("eager: held step holds, no hold output", "");
    td_up(4);
    expect("eager: held step releases with the key", "-60 ");
    advance(50); td_down(4); td_up(4);
    expect("eager: chain closed while held restarts", "+60 -60 ");
    idle();
    flask_tapdance_release_set(4, 50);
    td_down(4); advance(10); td_up(4); advance(60); td_down(4); td_up(4);
    expect("eager: release window shortens the chain", "+60 -60 +60 -60 ");
    advance(20); td_down(4); td_up(4);
    expect("eager: inside release window continues", "+61 -61 ");
    flask_tapdance_release_set(4, 0);
    idle();
    slots_dirty = BIT64(4);
    flask_tapdance_save();
    memset(&s, 0, sizeof(s));
    assert(flask_tapdance_settings_restore("s9", saved_len, read_buf, saved_buf) == 0);
    flask_tapdance_slot_get(9, &s);
    check("eager flag persists with the slot", s.flags == FLASK_TD_FLAG_EAGER && s.term_ms == 400, "");

    /* Fixed-slot dance instances are dances too (recursion guard). */
    check("fixed-slot instance is a dance", flask_behavior_id_is_tapdance(ID_SDEL) &&
                                                flask_behavior_id_is_dispatcher(ID_SDEL), "");
    set_out(13, 0, false, FLASK_TD_OUT_BEHAVIOR, ID_SDEL, 0);
    flask_tapdance_slot_get(13, &s);
    check("fixed-slot instance refused as output", s.taps[0].action == FLASK_TD_OUT_NONE, "");

    /* Compiled defaults: seed only slots settings never held. */
    {
        static const struct td_default defs[] = {
            {.slot = 13, .term_ms = 400, .flags = FLASK_TD_FLAG_EAGER, .n_bind = 2,
             .b = {{"zmk_behavior_key_press", 0x7002A, 0}, {"wdel_mm", 0, 0}}},
            {.slot = 14, .term_ms = 400, .n_bind = 1, .b = {{"zmk_behavior_key_press", 4, 0}}},
            {.slot = 12, .release_ms = 5000, .n_bind = 2, .b = {{"late", 7, 0}, {"sdel_td", 0, 0}}},
        };
        struct flask_tapdance_slot saved14 = {.term_ms = 300};
        uint8_t keep[sizeof(struct flask_tapdance_slot)];

        for (int i = 12; i <= 14; i++) {
            memset(&cfg.slots[i], 0, sizeof(cfg.slots[i]));
            slots_saved &= ~BIT64(i);
            slots_seeded &= ~BIT64(i);
        }
        slots_dirty = 0;
        saved14.taps[0] = (struct flask_tapdance_output){.action = FLASK_TD_OUT_USAGE, .param1 = 9};
        memcpy(keep, &saved14, sizeof(keep));
        assert(flask_tapdance_settings_restore("s14", sizeof(keep), read_buf, keep) == 0);

        rc = seed_defaults(defs, ARRAY_SIZE(defs));
        check("seed: unresolved id reported", rc == 1, "");
        flask_tapdance_slot_get(13, &s);
        check("seed: absent slot seeded", s.term_ms == 400 && s.flags == FLASK_TD_FLAG_EAGER &&
                                              s.taps[0].action == FLASK_TD_OUT_USAGE &&
                                              s.taps[0].param1 == 0x7002A &&
                                              s.taps[1].behavior_id == ID_WDEL,
              "");
        check("seed: not dirty (not written to flash)", slots_dirty == 0, "");
        flask_tapdance_slot_get(14, &s);
        check("seed: saved slot wins", s.term_ms == 300 && s.taps[0].param1 == 9, "");
        flask_tapdance_slot_get(12, &s);
        check("seed: unresolved slot left alone", s.taps[0].action == FLASK_TD_OUT_NONE, "");
        late_ok = true;
        flask_tapdance_term_set(13, 250);
        rc = seed_defaults(defs, ARRAY_SIZE(defs));
        flask_tapdance_slot_get(12, &s);
        check("seed: retry resolves, clamps, refuses dance output",
              rc == 0 && s.taps[0].behavior_id == ID_LATE && s.release_ms == 1000 &&
                  s.taps[1].action == FLASK_TD_OUT_NONE,
              "");
        flask_tapdance_slot_get(13, &s);
        check("seed: idempotent, app edit kept", s.term_ms == 250, "");

        /* App empties the defaulted slot: SAVE writes a tombstone, not a
         * delete, so the next boot does not seed it back. */
        for (int t = 0; t < 2; t++) {
            set_out(13, t, false, FLASK_TD_OUT_NONE, 0, 0);
        }
        flask_tapdance_term_set(13, 0);
        flask_tapdance_flags_set(13, 0);
        slots_dirty = BIT64(13);
        deletes = 0;
        flask_tapdance_save();
        check("seed: emptied default saves a tombstone",
              deletes == 0 && strcmp(saved_key, "flask/tapdance/s13") == 0 &&
                  saved_len == sizeof(s),
              "");
        memset(&cfg.slots[13], 0, sizeof(cfg.slots[13]));
        slots_saved &= ~BIT64(13);
        slots_seeded &= ~BIT64(13);
        assert(flask_tapdance_settings_restore("s13", saved_len, read_buf, saved_buf) == 0);
        seed_defaults(defs, ARRAY_SIZE(defs));
        flask_tapdance_slot_get(13, &s);
        check("seed: tombstone survives reboot", s.term_ms == 0 && s.taps[0].action == 0, "");
    }

    /* balance: every +X has a later -X */
    idle();
    {
        char buf[sizeof(full_log)];
        char *tok, *save;
        char names[128][24];
        int cnt[128] = {0}, n = 0;
        bool ok = true;

        strcpy(buf, full_log);
        for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            int k;

            for (k = 0; k < n && strcmp(names[k], tok + 1); k++) {}
            if (k == n) {
                snprintf(names[n++], sizeof(names[0]), "%s", tok + 1);
            }
            cnt[k] += tok[0] == '+' ? 1 : -1;
            if (cnt[k] < 0 || cnt[k] > 1) {
                ok = false;
                printf("  unbalanced at %s%s\n", tok[0] == '+' ? "+" : "-", names[k]);
            }
        }
        for (int k = 0; k < n; k++) {
            if (cnt[k] != 0) {
                ok = false;
                printf("  stuck: %s (%d)\n", names[k], cnt[k]);
            }
        }
        check("whole log balanced (+N before -N, none stuck)", ok, "");
        for (int i = 0; i < TD_MAX_HELD; i++) {
            ok = ok && dances[i].position == TD_POSITION_FREE && !dances[i].release_timer.pending;
        }
        check("no dance or timer left active", ok, "");
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
