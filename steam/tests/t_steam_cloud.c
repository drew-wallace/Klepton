#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "../tools/steam_probe_cloud.h"
static int sends, releases, ready, result = 1;
static uint32_t response_size = 3;
static const char *expected_method = "Cloud.GetAppFileChangelist#1";
static uint64_t send_method(void *self, const char *method, const void *body, uint32_t size, uint64_t context) {
    assert(self && !strcmp(method, expected_method));
    assert(size == 2 && !memcmp(body, "hi", 2) && !context); sends++; return 99;
}
static _Bool response_info(void *self, uint64_t handle, uint32_t *size, int *value) {
    assert(self && handle == 99); *size = response_size; *value = result; return ready;
}
static _Bool response_data(void *self, uint64_t handle, void *body, uint32_t size, _Bool release) {
    assert(self && handle == 99 && size == 3 && !release); memcpy(body, "yes", 3); return 1;
}
static _Bool release_method(void *self, uint64_t handle) { assert(self && handle == 99); releases++; return 1; }
static void *methods[] = {(void *)send_method, (void *)response_info, (void *)response_data, (void *)release_method};
static void **messages = methods;
static void *get_interface(void *self, int32_t user, int32_t pipe, const char *version) {
    assert(self && user == 1 && pipe == 2 && !strcmp(version, "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001")); return &messages;
}
static int automatic_cloud, cache_opened, cache_closed, cache_cancelled, chunks;
static _Bool cloud_enabled(void *self) { return automatic_cloud; }
static uint64_t cache_open(void *self, const char *name) { assert(!strcmp(name, "save.bin")); cache_opened++; return 100; }
static _Bool cache_chunk(void *self, uint64_t stream, const void *bytes, int32_t size) {
    assert(stream == 100 && size == 5 && !memcmp(bytes, "hello", 5)); chunks++; return 1;
}
static _Bool cache_close(void *self, uint64_t stream) { assert(stream == 100); cache_closed++; return 1; }
static _Bool cache_cancel(void *self, uint64_t stream) { cache_cancelled++; return 1; }
int main(void) {
    void *table[13] = {0}; table[12] = (void *)get_interface; void **client = table;
    char bytes[16] = {0}; uint32_t size = 999; int32_t value = 0;
    assert(!kl_steam_cloud_submit(7, 1, "hi", 2)); // no account
    cloud_tick(&client, 1, 2, 7, 0); assert(kl_steam_cloud_account() == 7);
    assert(!kl_steam_cloud_submit(8, 1, "hi", 2));
    assert(!kl_steam_cloud_submit(7, 7, "hi", 2));
    assert(!kl_steam_cloud_submit(7, 1, NULL, 2));
    assert(!kl_steam_cloud_submit(7, 1, "hi", CLOUD_RPC_LIMIT + 1));
    uint64_t token = kl_steam_cloud_submit(7, 1, "hi", 2); assert(token);
    assert(!kl_steam_cloud_submit(7, 1, "hi", 2));
    cloud_tick(&client, 1, 2, 7, 0); assert(sends == 1);
    assert(!kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value));
    ready = 1; cloud_tick(&client, 1, 2, 7, 1);
    assert(kl_steam_cloud_poll(token, bytes, 1, &size, &value) == -1 && size == 3);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 1 && !memcmp(bytes, "yes", 3));
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == -1);
    assert(releases == 1);
    // Batch completion must use the request/reply RPC. The nonblocking method
    // is a notification and would leave this mailbox waiting for a response.
    expected_method = "Cloud.CompleteAppUploadBatchBlocking#1";
    response_size = 0; // A successful protobuf response may have no fields.
    token = kl_steam_cloud_submit(7, 6, "hi", 2);
    cloud_tick(&client, 1, 2, 7, 1.5);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 1 && size == 0);
    response_size = 3;
    expected_method = "Cloud.GetAppFileChangelist#1";
    const char *http_methods[] = {"Cloud.BeginHTTPUpload#1", "Cloud.CommitHTTPUpload#1"};
    for (int i = 0; i < 2; i++) {
        expected_method = http_methods[i];
        token = kl_steam_cloud_submit(7, 8+i, "hi", 2); assert(token);
        cloud_tick(&client, 1, 2, 7, 1.6+i/10.0);
        assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 1);
    }
    assert(!kl_steam_cloud_submit(7, 11, "hi", 2));
    expected_method = "Cloud.GetAppFileChangelist#1";
    // A failure after success must not return the previous response/length.
    token = kl_steam_cloud_submit(7, 1, "hi", 2); result = 15;
    cloud_tick(&client, 1, 2, 7, 2);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 15 && size == 0);
    // Disconnect/account changes invalidate queued and active requests.
    token = kl_steam_cloud_submit(7, 1, "hi", 2);
    cloud_tick(&client, 1, 2, 8, 3);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 2 && size == 0);
    token = kl_steam_cloud_submit(8, 1, "hi", 2); ready = 0;
    cloud_tick(&client, 1, 2, 8, 4); cloud_tick(&client, 1, 2, 0, 5);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 2);
    cloud_tick(&client, 1, 2, 8, 5);
    token = kl_steam_cloud_submit(8, 1, "hi", 2);
    cloud_tick(&client, 1, 2, 8, 6); cloud_tick(&client, 1, 2, 8, 37);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 16);
    ready = 1; result = 1; response_size = CLOUD_RPC_LIMIT + 1;
    token = kl_steam_cloud_submit(8, 1, "hi", 2); cloud_tick(&client, 1, 2, 8, 38);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 2);
    // Restore API saves into the genuine SDK cache with its network switch off.
    char source[] = "/tmp/klepton-cloud-cache-XXXXXX";
    int fd = mkstemp(source); assert(fd >= 0 && write(fd, "hello", 5) == 5); close(fd);
    void *storage_table[24] = {0};
    storage_table[9] = (void *)cache_open; storage_table[10] = (void *)cache_chunk;
    storage_table[11] = (void *)cache_close; storage_table[12] = (void *)cache_cancel;
    storage_table[22] = (void *)cloud_enabled; void **storage = storage_table;
    cloud_cache_remote = &storage;
    assert(!kl_steam_cloud_cache(8, "../escape", source, 5));
    assert(!kl_steam_cloud_cache(8, "a//b", source, 5));
    token = kl_steam_cloud_cache(8, "save.bin", source, 5); assert(token);
    assert(!kl_steam_cloud_release_startup(8)); // Cannot hand off with a queued cache write.
    cloud_tick(&client, 1, 2, 8, 39);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 1);
    assert(cache_opened == 1 && cache_closed == 1 && chunks == 1 && !cache_cancelled);
    automatic_cloud = 1;
    token = kl_steam_cloud_cache(8, "save.bin", source, 5); cloud_tick(&client, 1, 2, 8, 40);
    assert(kl_steam_cloud_poll(token, bytes, sizeof bytes, &size, &value) == 1 && value == 2 && cache_opened == 1);
    assert(kl_steam_cloud_release_startup(8));
    unlink(source);
    puts("Steam Cloud mailbox and SDK cache: passed");
}
