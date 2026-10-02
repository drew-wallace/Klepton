#pragma once
#include <string.h>

// Defaults audited on this APK; the implementation is shared by other guests.
static inline int kl_walkabout_compat(const char *target) {
    return target && strcmp(target, "walkabout-57013") == 0;
}
