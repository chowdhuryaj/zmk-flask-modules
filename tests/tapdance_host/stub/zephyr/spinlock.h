#pragma once
struct k_spinlock { int unused; };
#define K_SPINLOCK(l) for (int _once = ((void)(l), 1); _once; _once = 0)
