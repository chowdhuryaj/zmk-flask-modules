zephyr_include_directories(include)

# flask_rgb runs on BOTH split halves — each renders its own strip slice;
# the role-specific split-sync sources are gated below.
target_sources_ifdef(CONFIG_ZMK_FLASK_RGB app PRIVATE src/flask_rgb.c)

if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL app PRIVATE src/input_processor_flask_scroll.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL app PRIVATE src/input_processor_flask_autoscroll.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL app PRIVATE src/input_processor_flask_accel.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSCALE app PRIVATE src/input_processor_flask_scrollscale.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP app PRIVATE src/input_processor_flask_scrollsnap.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP app PRIVATE src/input_processor_flask_ballswap.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_BALLSWAP app PRIVATE src/behavior_flask_ballswap.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_AUTOSCROLL app PRIVATE src/behavior_flask_autoscroll.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_RGB app PRIVATE src/behavior_flask_rgb.c)
  target_sources_ifdef(CONFIG_ZMK_FLASK_PROTO app PRIVATE src/flask_proto.c)
  # Raw-key half of layer autoexit: BEFORE combos so it sees combo keys the
  # combo engine swallows (a combo-held layer must count as held). Its
  # press half links after flask_holdtap below.
  target_sources_ifdef(CONFIG_ZMK_FLASK_LAYER_AUTOEXIT app PRIVATE src/flask_layer_autoexit.c)
  # After flask_proto.c on purpose: event subscriptions run in link order,
  # and the HUD keystate listener (flask_proto.c) must see raw position
  # events BEFORE flask_combos captures them.
  target_sources_ifdef(CONFIG_ZMK_FLASK_COMBOS app PRIVATE src/flask_combos.c)
  # csk links after combos: it must see the keycode events the combo
  # engine's typed outputs raise, not just raw taps.
  target_sources_ifdef(CONFIG_ZMK_FLASK_CSK app PRIVATE src/flask_csk.c)
  # adaptive's last-key tracker links after csk for the same reason: it must
  # see the final keycode events (csk's shifted substitutions included).
  target_sources_ifdef(CONFIG_ZMK_FLASK_ADAPTIVE app PRIVATE src/flask_adaptive.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_ADAPTIVE app PRIVATE src/behavior_flask_adaptive.c)
  target_sources_ifdef(CONFIG_ZMK_FLASK_TAPDANCE app PRIVATE src/flask_tapdance.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_TAPDANCE app PRIVATE src/behavior_flask_tapdance.c)
  target_sources_ifdef(CONFIG_ZMK_FLASK_MACROS app PRIVATE src/flask_macros.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_MACROS app PRIVATE src/behavior_flask_macros.c)
  # flask_leader's listener must run AFTER flask_proto's keystate listener
  # (same link-order rule as combos) and after combos' capture.
  target_sources_ifdef(CONFIG_ZMK_FLASK_LEADER app PRIVATE src/flask_leader.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_LEADER app PRIVATE src/behavior_flask_leader.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES app PRIVATE src/input_processor_flask_gestures.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_GESTURES app PRIVATE src/behavior_flask_gestures.c)
  # Links after combos/leader on purpose: the automouse position listener
  # must see key events AFTER those capture engines, so its latch-swallow
  # decision matches what actually reaches the keymap.
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOMOUSE app PRIVATE src/input_processor_flask_automouse.c)
  # Holdtap is LAST among the central listeners except autoexit and
  # autoshift (below): core hold-tap's listener sits after every module
  # listener (module sources link before zmk's own), and this port must hold
  # the same relative position — after combos/leader/automouse capture,
  # before core hold-tap and the keymap.
  target_sources_ifdef(CONFIG_ZMK_FLASK_HOLDTAP app PRIVATE src/behavior_flask_holdtap.c)
  # After flask_holdtap, before core hold-tap and the keymap: the autoexit
  # decision sees presses as the keymap will (combo keys swallowed,
  # hold-tap captures replayed after the decision).
  target_sources_ifdef(CONFIG_ZMK_FLASK_LAYER_AUTOEXIT app PRIVATE src/flask_layer_autoexit_press.c)
  # Autoshift links AFTER holdtap and autoexit on purpose: it must see
  # positions after holdtap's capture/replay and before the keymap (it
  # swallows &kp presses the keymap would otherwise type, so autoexit must
  # already have seen them; its layer walk then matches the post-exit state).
  target_sources_ifdef(CONFIG_ZMK_FLASK_AUTOSHIFT app PRIVATE src/flask_autoshift.c)
  if (CONFIG_ZMK_FLASK_RGB AND CONFIG_ZMK_SPLIT_BLE)
    target_sources(app PRIVATE src/flask_rgb_split_central.c)
  endif()
else()
  if (CONFIG_ZMK_FLASK_RGB AND CONFIG_ZMK_SPLIT_BLE)
    target_sources(app PRIVATE src/flask_rgb_split_peripheral.c)
  endif()
endif()
