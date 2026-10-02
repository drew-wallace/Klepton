#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "kl_jni.h"
#include "kl_obbmap.h"
#include "../runtime/jni/kl_jni_int.h"

static void expect_path(const char *root, const char *suffix) {
    char expected[1024];
    snprintf(expected, sizeof expected, "%s/%s", root, suffix);
    assert(!strcmp(kl_jni_obb_dir(), expected));
}
static void expect_asset(const char *path, const char *bytes) {
    long long size = -1;
    assert(kl_obbmap_stat(path, &size) && size == (long long)strlen(bytes));
    FILE *f = kl_obbmap_fopen(path, "rb");
    assert(f);
    char buffer[128] = {0};
    assert(fread(buffer, 1, sizeof buffer, f) == strlen(bytes));
    assert(!memcmp(buffer, bytes, strlen(bytes)));
    assert(fclose(f) == 0);
    int fd = kl_obbmap_open(path, O_RDONLY), handled = 0;
    assert(fd >= 0);
    memset(buffer, 0, sizeof buffer);
    ssize_t n = kl_obbmap_pread(fd, buffer, sizeof buffer, 0, &handled);
    // Deflated entries become ordinary temporary fds; stored entries are windows.
    if (!handled) n = pread(fd, buffer, sizeof buffer, 0);
    assert(n == (ssize_t)strlen(bytes));
    assert(!memcmp(buffer, bytes, strlen(bytes)));
    kl_obbmap_close(fd, &handled);
    if (!handled) close(fd);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    char assets[1024]; long version = 0; const char *version_name = NULL;
    snprintf(assets, sizeof assets, "%s/assets", argv[1]);
    kl_jni_set_assets_dir(assets);
    kl_jni_guest_version(&version, &version_name);
    assert(version == 111 && !strcmp(version_name, "probe"));
    assert(!strcmp(kl_jni_guest_package(), "test.probe"));
    assert(!strcmp(kl_jni_manifest_meta("fixture.context"), "probe"));
    const char *why = NULL;
    assert(klj_permission_state("android.permission.INTERNET", &why));
    assert(!klj_permission_state("android.permission.RECORD_AUDIO", &why));
    // Model a Steam probe touching JNI paths before the game is configured.
    kl_jni_set_obb_rel("obb");
    kl_jni_set_files_dir(argv[1]);
    expect_path(argv[1], "obb");
    kl_jni_set_files_dir(argv[2]);
    snprintf(assets, sizeof assets, "%s/assets", argv[2]);
    kl_jni_set_assets_dir(assets);
    kl_jni_guest_version(&version, &version_name);
    assert(version == 57013 && !strcmp(version_name, "game"));
    assert(!strcmp(kl_jni_guest_package(), "test.game"));
    assert(!strcmp(kl_jni_manifest_meta("fixture.context"), "game"));
    assert(klj_permission_state("android.permission.RECORD_AUDIO", &why));
    assert(!klj_permission_state("android.permission.INTERNET", &why));
    expect_path(argv[2], "obb");
    kl_jni_set_obb_rel("Android/obb/test.game");
    expect_path(argv[2], "Android/obb/test.game");
    kl_jni_set_obb_rel("obb");
    expect_path(argv[2], "obb");
    kl_obbmap_init(kl_jni_obb_dir(), "");
    expect_asset("jar:file:///game.apk!/assets/aa/settings.json", "{\"fixture\":true}");
    expect_asset("/game.apk/assets/bin/Data/resource", "packaged settings fixture");
    assert(!kl_obbmap_stat("jar:file:///game.apk!/assets/aa/absent.json", NULL));
    void *directory = kl_obbmap_opendir("/game.apk/assets/bin/Data/UnitySubsystems/");
    assert(directory);
    char name[256]; int is_directory = 0;
    assert(kl_obbmap_readdir(directory, name, sizeof name, &is_directory) == 1);
    assert(!strcmp(name, "UnityOpenXR") && is_directory);
    assert(kl_obbmap_readdir(directory, name, sizeof name, &is_directory) == 0);
    assert(kl_obbmap_closedir(directory) == 0);
    puts("Steam-to-game path handoff and stored/deflated OBB asset reads passed");
}
