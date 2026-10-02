#include <assert.h>
#include <stdio.h>
#include "../tools/steam_probe_ticket_gate.h"
static probe_ticket_gate setup(void) {
    probe_ticket_gate gate = {.user = 7, .session_handle = 41, .web_handle = 42, .session_size = 3};
    memcpy(gate.session_bytes, "abc", 3); return gate;
}
int main(void) {
    probe_ticket_gate gate = setup();
    probe_session_ticket_response session = {.handle=41, .result=1};
    probe_web_ticket_response web = {.handle=42, .result=1, .size=3, .bytes={'x','y','z'}};
    assert(!probe_tickets_delivered(&gate)); // Handles/bytes alone are insufficient.
    assert(!probe_ticket_callback(&gate, 8, 163, &session, sizeof session));
    assert(!probe_ticket_callback(&gate, 7, 101, &session, sizeof session));
    session.handle = 100;
    assert(!probe_ticket_callback(&gate, 7, 163, &session, sizeof session)); session.handle=41;
    assert(probe_ticket_callback(&gate, 7, 163, &session, 7) == -1);
    assert(probe_ticket_callback(&gate, 7, 163, NULL, 8) == -1);
    assert(probe_ticket_callback(&gate, 7, 163, &session, 8) == 1);
    assert(!probe_tickets_delivered(&gate));
    assert(!probe_ticket_callback(&gate, 7, 163, &session, 8)); // Duplicate.
    web.size = -1;
    assert(probe_ticket_callback(&gate, 7, 168, &web, sizeof web) == -1);
    web.size = 2561;
    assert(probe_ticket_callback(&gate, 7, 168, &web, sizeof web) == -1);
    web.size = 0;
    assert(probe_ticket_callback(&gate, 7, 168, &web, sizeof web) == -1);
    web.size = 3;
    assert(probe_ticket_callback(&gate, 7, 168, &web, sizeof web-1) == -1);
    assert(probe_ticket_callback(&gate, 7, 168, &web, sizeof web) == 1);
    memset(web.bytes, 0, sizeof web.bytes); // Borrowed buffer is gone; owned copy survives.
    assert(!memcmp(gate.web_bytes, "xyz", 3));
    assert(probe_tickets_delivered(&gate));
    gate = setup(); session.result = 5;
    assert(probe_ticket_callback(&gate, 7, 163, &session, 8) == 1);
    assert(gate.session_size == 0 && gate.session_bytes[0] == 0);
    assert(!probe_tickets_delivered(&gate));
    gate = setup(); gate.session_size = 0; session.result = 1;
    assert(probe_ticket_callback(&gate, 7, 163, &session, 8) == 1);
    assert(!probe_tickets_delivered(&gate));
    gate = setup(); web.result = 5; web.size = 0;
    assert(probe_ticket_callback(&gate, 7, 168, &web, sizeof web) == 1);
    assert(!gate.web_size && !probe_tickets_delivered(&gate));
    puts("Steam ticket callback matching, ownership, bounds and success gates passed (offline fixtures)");
}
