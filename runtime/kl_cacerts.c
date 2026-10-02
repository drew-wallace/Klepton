//
// The root CA anchors, and the one decision behind them.
//
// The guest's TLS is unitytls, and on Android it builds its CA bundle out of
// the system trust store over JNI. With no store to read, every chain came back
// UNITYTLS_X509VERIFY_FLAG_NOT_TRUSTED and VRChat's login reported
// "Failed to connect to VRChat" (Curl error 60 in its own log).
//
// There were three answers available and only one of them is honest:
//
//   nothing        an empty TrustManager[] — what this runtime did until now.
//                  Correct in the sense that it invents nothing, and it makes
//                  HTTPS structurally impossible.
//   trust-all      a manager that approves everything. Refused, and it stays
//                  refused: that is not a shim answering for a missing
//                  platform, it is us silently disabling a check the guest
//                  believes it is making.
//   the host's own the real anchors this machine trusts. Validation is
//                  untouched — unitytls still does the whole chain check, and
//                  libunity asks for `getAcceptedIssuers` and never
//                  `checkServerTrusted`, so the guest takes a trust SET from us
//                  and reaches its own verdict. We answer "which roots exist",
//                  which is exactly what the Android call means.
//
// The table is baked (kl_cacert_table.h, `make cacerts`) rather than read live,
// because SecTrustCopyAnchorCertificates is __IPHONE_NA: macOS can enumerate
// its trust store and visionOS cannot. Reading it where it exists would leave
// the device on a different set of roots from the host — a divergence that
// would show up as "logs in on the Mac, will not log in on the headset", which
// is the most expensive shape of bug this project has.
//

#include "kl_cacerts.h"

#include <stdio.h>

#include "kl_env.h"
#include "kl_cacert_table.h"

// KL_CA_ANCHORS=0 restores the failing configuration exactly — an empty trust
// store, i.e. the A/B for "is this what is blocking the connection?".
static int enabled(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_CA_ANCHORS", 1);
        if (!on)
            fprintf(stderr, "[ca] KL_CA_ANCHORS=0 — presenting NO trust store; "
                            "guest TLS will fail every chain\n");
    }
    return on;
}

int kl_cacert_count(void) {
    return enabled() ? KL_CACERT_COUNT : 0;
}

const unsigned char *kl_cacert_at(int i, size_t *len) {
    if (!enabled() || i < 0 || i >= KL_CACERT_COUNT) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = kl_cacert_span[i].len;
    return kl_cacert_der + kl_cacert_span[i].off;
}

int kl_cacerts_write_pem(FILE *output) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!output) return -1;
    for (int i = 0; i < kl_cacert_count(); i++) {
        size_t length;
        const unsigned char *bytes = kl_cacert_at(i, &length);
        if (fputs("-----BEGIN CERTIFICATE-----\n", output) == EOF) return -1;
        unsigned column = 0;
        for (size_t j = 0; j < length; j += 3) {
            size_t remain = length - j;
            unsigned value = (unsigned)bytes[j] << 16;
            if (remain > 1) value |= (unsigned)bytes[j + 1] << 8;
            if (remain > 2) value |= bytes[j + 2];
            char encoded[] = { alphabet[(value >> 18) & 63], alphabet[(value >> 12) & 63],
                remain > 1 ? alphabet[(value >> 6) & 63] : '=', remain > 2 ? alphabet[value & 63] : '=' };
            if (fwrite(encoded, 1, 4, output) != 4) return -1;
            column += 4;
            if (column == 64) { if (fputc('\n', output) == EOF) return -1; column = 0; }
        }
        if (column && fputc('\n', output) == EOF) return -1;
        if (fputs("-----END CERTIFICATE-----\n", output) == EOF) return -1;
    }
    return ferror(output) ? -1 : 0;
}
