zephyr_include_directories(include)

# flask_rgb runs on BOTH split halves — each renders its own strip slice;
# the role-specific split-sync sources are gated below.
target_sources_ifdef(CONFIG_ZMK_FLASK_RGB app PRIVATE src/flask_rgb.c)

if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL app PRIVATE src/input_processor_flask_scroll.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_AUTOSCROLL app PRIVATE src/input_processor_flask_autoscroll.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_ACCEL app PRIVATE src/input_processor_flask_accel.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLLSNAP app PRIVATE src/input_processor_flask_scrollsnap.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_BALLSWAP app PRIVATE src/input_processor_flask_ballswap.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_BALLSWAP app PRIVATE src/behavior_flask_ballswap.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_AUTOSCROLL app PRIVATE src/behavior_flask_autoscroll.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_RGB app PRIVATE src/behavior_flask_rgb.c)
  target_sources_ifdef(CONFIG_ZMK_FLASK_PROTO app PRIVATE src/flask_proto.c)
  # After flask_proto.c on purpose: event subscriptions run in link order,
  # and the HUD keystate listener (flask_proto.c) must see raw position
  # events BEFORE flask_combos captures them.
  target_sources_ifdef(CONFIG_ZMK_FLASK_COMBOS app PRIVATE src/flask_combos.c)
  target_sources_ifdef(CONFIG_ZMK_FLASK_MACROS app PRIVATE src/flask_macros.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_MACROS app PRIVATE src/behavior_flask_macros.c)
  # flask_leader's listener must run AFTER flask_proto's keystate listener
  # (same link-order rule as combos) and after combos' capture.
  target_sources_ifdef(CONFIG_ZMK_FLASK_LEADER app PRIVATE src/flask_leader.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_LEADER app PRIVATE src/behavior_flask_leader.c)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_GESTURES app PRIVATE src/input_processor_flask_gestures.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_FLASK_GESTURES app PRIVATE src/behavior_flask_gestures.c)
  if (CONFIG_ZMK_FLASK_RGB AND CONFIG_ZMK_SPLIT_BLE)
    target_sources(app PRIVATE src/flask_rgb_split_central.c)
  endif()
else()
  if (CONFIG_ZMK_FLASK_RGB AND CONFIG_ZMK_SPLIT_BLE)
    target_sources(app PRIVATE src/flask_rgb_split_peripheral.c)
  endif()
endif()
