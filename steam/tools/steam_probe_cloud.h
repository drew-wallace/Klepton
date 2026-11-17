// Cloud RPC mailbox. Only the backend worker touches Valve interfaces.
// Public ABI: ISteamClient023::GetISteamGenericInterface (slot 12),
// ISteamUnifiedMessages001 (slots 0..3), Valve's steamworks_sdk_139 header.
#ifndef KL_STEAM_PROBE_CLOUD_H
#define KL_STEAM_PROBE_CLOUD_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#define CLOUD_RPC_LIMIT (8 * 1024 * 1024)
typedef struct { char name[1024], source[4096]; uint64_t size; } cloud_cache_request;
static void *cloud_cache_remote;
static uint64_t cloud_release_account;
static int cloud_release_requested;
static unsigned cloud_release_epoch, cloud_release_retry_epoch;
static pthread_mutex_t cloud_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t account, expected, serial, handle;
    int state, method, result; // 0 idle, 1 queued, 2 in flight, 3 finished
    unsigned char *request, *response;
    uint32_t request_size, response_size;
    double deadline;
} cloud_rpc;
int32_t kl_steam_cloud_release_startup(uint64_t account) {
    pthread_mutex_lock(&cloud_lock);
    int accepted = !cloud_rpc.state && account != 0 && cloud_rpc.account == account;
    if (accepted) { cloud_release_requested = 1; cloud_release_account = account; cloud_release_epoch++; }
    pthread_mutex_unlock(&cloud_lock); return accepted;
}
void kl_steam_cloud_cancel_startup(uint64_t account) {
    pthread_mutex_lock(&cloud_lock);
    if (cloud_release_account == account) cloud_release_requested = 0;
    pthread_mutex_unlock(&cloud_lock);
}
static const char *cloud_methods[] = {NULL,
    "Cloud.GetAppFileChangelist#1", "Cloud.ClientFileDownload#1",
    "Cloud.BeginAppUploadBatch#1", "Cloud.ClientBeginFileUpload#1",
    "Cloud.ClientCommitFileUpload#1", "Cloud.CompleteAppUploadBatchBlocking#1",
    NULL, "Cloud.BeginHTTPUpload#1", "Cloud.CommitHTTPUpload#1", "Cloud.ClientGetAppQuotaUsage#1"};
uint64_t kl_steam_cloud_account(void) {
    pthread_mutex_lock(&cloud_lock); uint64_t id = cloud_rpc.account;
    pthread_mutex_unlock(&cloud_lock); return id;
}
static uint64_t cloud_submit(uint64_t account, int32_t method, const void *bytes, uint32_t size) {
    if (!account || method < 1 || method > 10 || size > CLOUD_RPC_LIMIT || (size && !bytes)) return 0;
    pthread_mutex_lock(&cloud_lock);
    uint64_t serial = 0;
    if (!cloud_rpc.state && cloud_rpc.account == account) {
        unsigned char *copy = malloc(size ? size : 1);
        if (copy) {
            if (size) memcpy(copy, bytes, size);
            cloud_rpc.response_size = 0; cloud_rpc.result = 0;
            cloud_rpc.request = copy; cloud_rpc.request_size = size;
            cloud_rpc.expected = account; cloud_rpc.method = method; cloud_rpc.state = 1;
            serial = ++cloud_rpc.serial;
        }
    }
    pthread_mutex_unlock(&cloud_lock); return serial;
}
uint64_t kl_steam_cloud_submit(uint64_t account, int32_t method, const void *bytes, uint32_t size) {
    if (method == 7 || method > 10) return 0;
    return cloud_submit(account, method, bytes, size);
}
uint64_t kl_steam_cloud_cache(uint64_t account, const char *name, const char *source, uint64_t size) {
    if (!name || !source || !*name || name[0] == '/' || source[0] != '/' || size > 256ull * 1024 * 1024 ||
        strnlen(name, 1024) >= 1024 || strnlen(source, 4096) >= 4096) return 0;
    for (const char *part = name; *part;) {
        const char *end = strchr(part, '/'); size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 1 && part[0] == '.') || (length == 2 && !memcmp(part, "..", 2))) return 0;
        for (size_t i = 0; i < length; i++) if ((unsigned char)part[i] < 32 || part[i] == '\\' || part[i] == ':') return 0;
        if (!end) break;
        part = end + 1; if (!*part) return 0;
    }
    cloud_cache_request request = {.size = size};
    strcpy(request.name, name); strcpy(request.source, source);
    return cloud_submit(account, 7, &request, sizeof request);
}
// Write the verified bytes through the SDK while automatic Cloud is disabled.
// Its in-memory file index and on-disk remotecache must agree before handoff.
static int cloud_write_cache(void *remote, const cloud_cache_request *request) {
    if (!remote) return 2;
    void **v = *(void ***)remote;
    if (((_Bool (*)(void *))v[22])(remote)) return 2;
    FILE *file = fopen(request->source, "rb");
    if (!file) return 2;
    struct stat st;
    if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != request->size) {
        fclose(file); return 2;
    }
    uint64_t stream = ((uint64_t (*)(void *, const char *))v[9])(remote, request->name);
    if (stream == UINT64_MAX) { fclose(file); return 2; }
    unsigned char *buffer = malloc(1024 * 1024);
    uint64_t total = 0; int ok = buffer != NULL;
    while (ok && total < request->size) {
        size_t amount = request->size - total < 1024 * 1024 ? (size_t)(request->size - total) : 1024 * 1024;
        size_t count = fread(buffer, 1, amount, file);
        ok = count == amount && ((_Bool (*)(void *, uint64_t, const void *, int32_t))v[10])(remote, stream, buffer, (int32_t)count);
        total += count;
    }
    free(buffer); fclose(file);
    if (!ok) { ((_Bool (*)(void *, uint64_t))v[12])(remote, stream); return 2; }
    return ((_Bool (*)(void *, uint64_t))v[11])(remote, stream) ? 1 : 2;
}
// 0 pending; 1 complete (EResult and length returned); -1 invalid token/buffer.
int32_t kl_steam_cloud_poll(uint64_t serial, void *bytes, uint32_t capacity,
                           uint32_t *size, int32_t *result) {
    pthread_mutex_lock(&cloud_lock);
    int state = 0;
    if (!serial || serial != cloud_rpc.serial || !cloud_rpc.state) state = -1;
    else if (cloud_rpc.state == 3) {
        if (size) *size = cloud_rpc.response_size;
        if (result) *result = cloud_rpc.result;
        if (cloud_rpc.response_size > capacity || (cloud_rpc.response_size && !bytes)) state = -1;
        else {
            if (cloud_rpc.response_size) memcpy(bytes, cloud_rpc.response, cloud_rpc.response_size);
            free(cloud_rpc.response); cloud_rpc.response = NULL; cloud_rpc.state = 0; state = 1;
        }
    }
    pthread_mutex_unlock(&cloud_lock); return state;
}
static void cloud_complete(int result) {
    free(cloud_rpc.request); cloud_rpc.request = NULL;
    cloud_rpc.result = result; cloud_rpc.state = 3;
}
static void cloud_tick(void *client, int32_t user, int32_t pipe, uint64_t account, double now) {
    // The object belongs to this worker/pipe and remains valid through its life.
    static void *messages;
    pthread_mutex_lock(&cloud_lock);
    cloud_rpc.account = account;
    if (cloud_rpc.state == 1 && cloud_rpc.method == 7) {
        int result = 2;
        if (account && cloud_rpc.expected == account && cloud_rpc.request_size == sizeof(cloud_cache_request))
            result = cloud_write_cache(cloud_cache_remote, (const cloud_cache_request *)cloud_rpc.request);
        cloud_complete(result);
    }
    if (cloud_rpc.state == 1 || cloud_rpc.state == 2) {
        if (!messages && account) {
            void *(*get)(void *, int32_t, int32_t, const char *) = (void *)(*(void ***)client)[12];
            messages = get(client, user, pipe, "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001");
        }
        if (!messages || !account || cloud_rpc.expected != account) {
            if (cloud_rpc.handle && messages)
                ((_Bool (*)(void *, uint64_t))(*(void ***)messages)[3])(messages, cloud_rpc.handle);
            cloud_rpc.handle = 0; cloud_complete(2);
        } else {
            void **v = *(void ***)messages;
            if (cloud_rpc.state == 1) {
                cloud_rpc.handle = ((uint64_t (*)(void *, const char *, const void *, uint32_t, uint64_t))v[0])(
                    messages, cloud_methods[cloud_rpc.method], cloud_rpc.request, cloud_rpc.request_size, 0);
                free(cloud_rpc.request); cloud_rpc.request = NULL;
                cloud_rpc.deadline = now + 30; cloud_rpc.state = 2;
                if (!cloud_rpc.handle) cloud_complete(2);
            }
            if (cloud_rpc.state == 2) {
                uint32_t size = 0; int result = 0;
                _Bool ready = ((_Bool (*)(void *, uint64_t, uint32_t *, int *))v[1])(
                    messages, cloud_rpc.handle, &size, &result);
                if (ready || now >= cloud_rpc.deadline) {
                    if (!ready) result = 16; // timeout
                    if (size > CLOUD_RPC_LIMIT) result = 2;
                    if (result == 1 && size) {
                        cloud_rpc.response = malloc(size);
                        if (!cloud_rpc.response || !((_Bool (*)(void *, uint64_t, void *, uint32_t, _Bool))v[2])(
                            messages, cloud_rpc.handle, cloud_rpc.response, size, 0)) result = 2;
                        else cloud_rpc.response_size = size;
                    }
                    ((_Bool (*)(void *, uint64_t))v[3])(messages, cloud_rpc.handle);
                    cloud_rpc.handle = 0; cloud_complete(result);
                }
            }
        }
    }
    pthread_mutex_unlock(&cloud_lock);
}
#endif
