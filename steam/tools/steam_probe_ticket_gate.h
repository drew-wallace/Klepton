// Ticket delivery is distinct from server verification. These structures match
// SDK 1.63 POSIX callbacks. No consumer receives bytes before a matching success.
#ifndef KL_STEAM_PROBE_TICKET_GATE_H
#define KL_STEAM_PROBE_TICKET_GATE_H
#include <stdint.h>
#include <string.h>
#pragma pack(push, 4)
typedef struct { uint32_t handle; int32_t result; } probe_session_ticket_response;
typedef struct { uint32_t handle; int32_t result, size; uint8_t bytes[2560]; } probe_web_ticket_response;
#pragma pack(pop)
_Static_assert(sizeof(probe_session_ticket_response) == 8, "session ticket ABI");
_Static_assert(sizeof(probe_web_ticket_response) == 2572, "Web API ticket ABI");
typedef struct {
    int32_t user;
    uint32_t session_handle, web_handle, session_size, web_size;
    int session_done, web_done, session_result, web_result;
    uint8_t session_bytes[4096], web_bytes[2560];
} probe_ticket_gate;
// 0 unrelated, 1 matched callback, -1 invalid ABI/length. Copy borrowed data
// while owned by the dispatcher. Callers free that callback before advancing.
static int probe_ticket_callback(probe_ticket_gate *gate, int32_t user, int id, const void *payload, int size) {
    if (user != gate->user || (id != 163 && id != 168)) return 0;
    if (!payload) return -1;
    if (id == 163) {
        if (size != sizeof(probe_session_ticket_response)) return -1;
        probe_session_ticket_response response; memcpy(&response, payload, sizeof response);
        if (!gate->session_handle || response.handle != gate->session_handle || gate->session_done) return 0;
        gate->session_result = response.result; gate->session_done = 1;
        if (response.result != 1) { gate->session_size = 0; memset(gate->session_bytes, 0, sizeof gate->session_bytes); }
    } else {
        if (size != sizeof(probe_web_ticket_response)) return -1;
        // Read header scalars separately; do not make an extra secret copy of
        // the entire callback on the stack. Validate before copying its bytes.
        uint32_t handle; int32_t result, length;
        memcpy(&handle, payload, 4); memcpy(&result, (const char *)payload + 4, 4);
        memcpy(&length, (const char *)payload + 8, 4);
        if (!gate->web_handle || handle != gate->web_handle || gate->web_done) return 0;
        if (length < 0 || (size_t)length > sizeof gate->web_bytes || (result == 1 && !length)) return -1;
        gate->web_result = result; gate->web_done = 1;
        if (result == 1) { gate->web_size = (uint32_t)length; memcpy(gate->web_bytes, (const char *)payload + 12, (size_t)length); }
    }
    return 1;
}
static int probe_tickets_delivered(const probe_ticket_gate *gate) {
    return gate->session_done && gate->web_done && gate->session_result == 1 && gate->web_result == 1 &&
           gate->session_size > 0 && gate->session_size <= sizeof gate->session_bytes &&
           gate->web_size > 0 && gate->web_size <= sizeof gate->web_bytes;
}
#endif
