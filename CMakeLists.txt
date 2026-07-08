if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_FLASK_SCROLL app PRIVATE src/input_processor_flask_scroll.c)
endif()
