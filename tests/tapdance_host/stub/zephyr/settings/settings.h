#pragma once
#include <stddef.h>
#include <sys/types.h>
typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);
int settings_save_one(const char *name, const void *value, size_t val_len);
int settings_delete(const char *name);
