"""Select signing capabilities for each guest, with explicit build overrides."""

OPTIONAL_ENTITLEMENTS = {
    "com.apple.developer.kernel.extended-virtual-addressing": "KLEPTON_EXTENDED_VA",
    "com.apple.developer.low-latency-streaming": "KLEPTON_LOW_LATENCY",
}


def select_entitlements(authored, target, environ):
    # Steam Link benefits from the larger guest address space and the system's
    # low-latency wireless mode. Device signing requires a supporting team;
    # provisioning errors must remain visible instead of silently downgrading.
    default = "1" if target == "steamlink-vr" else "0"
    selected = dict(authored)
    dropped = []
    for key, variable in OPTIONAL_ENTITLEMENTS.items():
        if environ.get(variable, default) != "1":
            selected.pop(key, None)
            dropped.append(key)
    return selected, dropped
