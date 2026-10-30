#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "klepton.h"

int main(void) {
    int (*guest_putenv)(char *) = kl_shim_lookup("putenv");
    char *(*guest_getenv)(const char *) = kl_shim_lookup("getenv");
    assert(guest_putenv && guest_getenv);
    char setting[] = "KLEPTON_TEST_PUTENV=first";
    assert(guest_putenv(setting) == 0);
    assert(strcmp(guest_getenv("KLEPTON_TEST_PUTENV"), "first") == 0);
    // putenv stores the caller's buffer, unlike setenv.
    memcpy(strchr(setting, '=') + 1, "later", 5);
    assert(strcmp(guest_getenv("KLEPTON_TEST_PUTENV"), "later") == 0);
    char remove[] = "KLEPTON_TEST_PUTENV";
    assert(guest_putenv(remove) == 0);
    assert(guest_getenv(remove) == NULL);
    puts("guest putenv preserves storage and bare-name removal");
}
