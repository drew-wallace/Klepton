// Private ABI experiment for the exact Steam Frame 0.3.0 client. The builder
// enables this only after verifying the pinned input hash. Runtime address
// checks reject mismatched engine, proxy and legacy export layouts as well.
#ifndef KL_STEAM_PROBE_LOGIN_H
#define KL_STEAM_PROBE_LOGIN_H
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#ifdef KL_STEAM_LOGIN_ABI_PINNED
static int login_probe_address(kl_image *image, void *method, size_t expected) {
    size_t actual = 0;
    const char *owner = kl_addr_image(method, &actual);
    const char *reference = kl_addr_image(kl_sym(image, "CreateInterface"), NULL);
    return owner && reference && !strcmp(owner, reference) && actual == expected;
}

static int login_probe_loopback(uint32_t port) {
    if (!port || port > 65535) return 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); return 0; }
    struct sockaddr_in address = {0};
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int connected = connect(fd, (struct sockaddr *)&address, sizeof address) == 0;
    if (!connected && errno == EINPROGRESS) {
        struct pollfd wait = {.fd = fd, .events = POLLOUT};
        int error = -1;
        socklen_t length = sizeof error;
        connected = poll(&wait, 1, 1000) > 0 &&
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error;
    }
    close(fd);
    return connected;
}

// Standard RFC 6455 upgrade only. No Steam messages, credentials or local auth
// key are sent. Compare the native URLSession client with a minimal HTTP request.
static int login_probe_http_upgrade(uint32_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval timeout = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    struct sockaddr_in address = {0};
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int status = 0;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0) {
        char request[512];
        int length = snprintf(request, sizeof request,
            "GET /transportsocket/ HTTP/1.1\r\nHost: localhost:%u\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Origin: https://steamloopback.host\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n", port);
        if (length > 0 && (size_t)length < sizeof request &&
            send(fd, request, (size_t)length, 0) == length) {
            // Read the status line only; never dump headers or response bodies.
            char line[128]; size_t count = 0;
            while (count < sizeof line - 1 && recv(fd, line + count, 1, 0) == 1) {
                if (line[count++] == '\n') break;
            }
            line[count] = 0;
            if (sscanf(line, "HTTP/1.1 %d", &status) != 1) status = 0;
        }
    }
    close(fd);
    return status;
}

// CMsgWebUITransportInfo's embedded descriptor name at 0x7cebf0 establishes
// field 1 = uint32 port; field 2 = string auth_key. Audited generated ARM64
// layout: size 40, port at 32, ArenaStringPtr at 24 with low-bit tagging.
// Construct and destroy with Valve's own functions. No auth key is logged.
static void login_probe_transport(kl_image *image, void *engine, void *client, int32_t pipe) {
    void *getter = (*(void ***)engine)[14];
    if (!login_probe_address(image, getter, 0x1289258)) {
        fprintf(stderr, "[steam-probe] transport getter ABI mismatch\n");
        return;
    }
    void *(*get_utils)(void *, int32_t) = (void *)getter;
    void *utils = get_utils(engine, pipe);
    if (!utils || !login_probe_address(image, (*(void ***)utils)[106], 0x11232e4)) {
        fprintf(stderr, "[steam-probe] transport interface ABI mismatch\n");
        return;
    }
    unsigned char *base = kl_base(image);
    void (*construct)(void *, void *) = (void *)(base + 0x2044d64);
    void (*destroy)(void *) = (void *)(base + 0x2044d9c);
    if (!login_probe_address(image, (void *)construct, 0x2044d64) ||
        !login_probe_address(image, (void *)destroy, 0x2044d9c)) return;
    _Alignas(8) unsigned char message[40] = {0};
    construct(message, NULL);
    _Bool (*get_info)(void *, void *) = (void *)(*(void ***)utils)[106];
    _Bool result = get_info(utils, message);
    uint32_t port = 0;
    memcpy(&port, message + 32, sizeof port);
    uintptr_t field_pointer = 0;
    memcpy(&field_pointer, message + 24, sizeof field_pointer);
    const unsigned char *field = (void *)(field_pointer & ~(uintptr_t)1);
    size_t key_length = 0;
    if (field) {
        if (field[0] & 1) memcpy(&key_length, field + 8, sizeof key_length);
        else key_length = field[0] >> 1;
    }
    printf("[steam-probe] WebUI transport info: result=%d port=%u auth_key_present=%d\n",
           result, port, key_length > 0);
    // A successful TCP connect proves a local listener, not WebSocket
    // authentication, a Steam account session or receipt of a signed ticket.
    // GetInfo creates the listener lazily. Exercise host frames after creation
    // before making the transport request; do not block its startup on the test.
    if (result) {
        void (*frame)(void *) = (void *)(*(void ***)client)[19];
        const struct timespec interval = {0, 10000000};
        for (unsigned i = 0; i < 50; i++) { frame(client); nanosleep(&interval, NULL); }
        printf("[steam-probe] WebUI startup frame pump returned\n");
    }
    int reachable = result && login_probe_loopback(port);
    printf("[steam-probe] WebUI clientdll loopback reachable=%d\n", reachable);
    if (reachable)
        printf("[steam-probe] minimal WebSocket HTTP status=%d\n", login_probe_http_upgrade(port));
#if defined(KL_STEAM_PROBE_LIBRARY) && !defined(KL_STEAM_GAME_HOST)
    if (reachable) {
        extern int32_t kl_steam_probe_websocket(uint32_t);
        printf("[steam-probe] WebUI WebSocket upgraded=%d\n", kl_steam_probe_websocket(port));
    }
#endif
    destroy(message);
}

#endif

static int login_probe_without_credentials(kl_image *image, void *engine,
                                          void *steam_user, void *client, int32_t user, int32_t pipe) {
#ifndef KL_STEAM_LOGIN_ABI_PINNED
    (void)image; (void)engine; (void)steam_user; (void)client; (void)user; (void)pipe;
    fprintf(stderr, "[steam-probe] private login probe requires the hash-pinned recovery builder\n");
    return 1;
#else
    if (!engine || !steam_user) return 1;
    void *getter = (*(void ***)engine)[8];
    void *logon = kl_sym(image, "Steam_LogOn");
    void *logoff = kl_sym(image, "Steam_LogOff");
    void *connected = kl_sym(image, "Steam_BConnected");
    if (!login_probe_address(image, getter, 0x1286858) ||
        !login_probe_address(image, logon, 0xe54304) ||
        !login_probe_address(image, logoff, 0xe54398) ||
        !login_probe_address(image, connected, 0xe544c8)) {
        fprintf(stderr, "[steam-probe] private login ABI mismatch\n");
        return 1;
    }
    void *(*get_user)(void *, int32_t, int32_t) = (void *)getter;
    void *internal = get_user(engine, user, pipe);
    if (!internal) return 1;
    void **methods = *(void ***)internal;
    // Independent proxy, dispatcher, secondary-vtable and backend-body audit.
    const struct { unsigned slot; size_t offset; } expected[] = {
        {0, 0x10ceb50}, {1, 0x10ceb58}, {3, 0x10cef30},
        {4, 0x10cf0d4}, {5, 0x10cf2a4}, {6, 0x10cf474},
        {9, 0x10cf9e4}, {54, 0x10d5368}, {56, 0x10d5704}
    };
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        if (!login_probe_address(image, methods[expected[i].slot], expected[i].offset)) {
            fprintf(stderr, "[steam-probe] IClientUser ABI mismatch at slot %u\n", expected[i].slot);
            return 1;
        }
    }
    int32_t (*state)(void *) = (void *)methods[5];
    _Bool (*trying)(void *) = (void *)methods[9];
    _Bool (*is_connected)(int32_t, int32_t) = (void *)connected;
    uint64_t (*steam_id)(void *) = (void *)(*(void ***)steam_user)[2];
    int32_t (*login)(int32_t, int32_t, uint64_t) = (void *)logon;
    void (*logout)(int32_t, int32_t) = (void *)logoff;
    printf("[steam-probe] native login ABI verified; state=%d trying=%d connected=%d\n",
           state(internal), trying(internal), is_connected(user, pipe));
    fflush(stdout);
    // Preserve the backend's own current SteamID. No account identity is invented
    // and no credential/token setter is called. The expected local rejection is
    // k_EResultInvalidPassword (5); success is NOT the expected result.
    int32_t result = login(user, pipe, steam_id(steam_user));
    printf("[steam-probe] credential-free Steam_LogOn result=%d state=%d trying=%d\n",
           result, state(internal), trying(internal));
    fflush(stdout);
    logout(user, pipe);
    printf("[steam-probe] native LogOff returned\n");
    login_probe_transport(image, engine, client, pipe);
    return result != 5;
#endif
}
#endif
