# Host check for flask_tapdance.c against stub/ Zephyr/ZMK headers: make
CFLAGS = -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror -g \
	-Istub -I../../include \
	-DCONFIG_ZMK_FLASK_TAPDANCE_SLOTS=16 -DCONFIG_ZMK_FLASK_TAPDANCE_TAPS=4 \
	-DCONFIG_ZMK_BEHAVIOR_LOCAL_IDS=1 -DCONFIG_KERNEL_INIT_PRIORITY_DEFAULT=0

run: tapdance_test
	./tapdance_test

tapdance_test: test.c ../../src/flask_tapdance.c ../../include/flask_tapdance/flask_tapdance.h ../../include/flask_loop_guard.h
	$(CC) $(CFLAGS) -o $@ test.c
