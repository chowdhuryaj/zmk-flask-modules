#pragma once
#define ZMK_INPUT_PROC_CONTINUE 0
#define ZMK_INPUT_PROC_STOP 1
struct zmk_input_processor_state;
struct zmk_input_processor_driver_api { void *handle_event; };
