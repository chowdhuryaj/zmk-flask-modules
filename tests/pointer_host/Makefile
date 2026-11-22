# Host check for flask_scroll's vertical priority and flask_smooth: make
# (own stubs first, then the tapdance host's Zephyr stubs)
CFLAGS = -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Werror -g \
	-Istub -I../tapdance_host/stub -I../../include

run: pointer_test
	./pointer_test

pointer_test: test.c ../../src/input_processor_flask_scroll.c ../../src/input_processor_flask_smooth.c
	$(CC) $(CFLAGS) -o $@ test.c
