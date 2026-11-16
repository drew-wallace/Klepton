#include <assert.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "kl_jni.h"
#include "klepton.h"
#include "kl_obbmap.h"
#include "kl_ndk.h"
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
    // ZIP readers seek to the end before reading the central directory.
    assert(!fseek(f,0,SEEK_END) && ftell(f)==(long)strlen(bytes));
    assert(!fseek(f,0,SEEK_SET));
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
static void expect_ndk_asset_seek(const char *root) {
    char directory[1024], path[1200];
    snprintf(directory, sizeof directory, "%s/assets", root);
    snprintf(path, sizeof path, "%s/seek.bin", directory);
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite("0123456789", 1, 10, f) == 10);
    // A sparse fixture catches accidental 32-bit argument/return truncation.
    const int64_t tail = INT64_C(0x100000007);
    assert(!fseeko(f, tail, SEEK_SET) && fputc('z', f) == 'z');
    assert(!fclose(f));
    kl_ndk_set_assets_dir(directory);
    void *(*open_asset)(void *, const char *, int) = kl_shim_lookup("AAssetManager_open");
    int64_t (*seek_asset)(void *, int64_t, int) = kl_shim_lookup("AAsset_seek");
    int64_t (*seek64)(void *, int64_t, int) = kl_shim_lookup("AAsset_seek64");
    int64_t (*length)(void *) = kl_shim_lookup("AAsset_getLength");
    int64_t (*length64)(void *) = kl_shim_lookup("AAsset_getLength64");
    int (*read_asset)(void *, void *, size_t) = kl_shim_lookup("AAsset_read");
    void (*close_asset)(void *) = kl_shim_lookup("AAsset_close");
    assert(open_asset && seek_asset && seek64 && length && length64 && read_asset && close_asset);
    void *asset = open_asset(kl_ndk_asset_manager(), "seek.bin", 2); assert(asset);
    assert(length(asset) == tail + 1 && length64(asset) == tail + 1);
    char bytes[4] = {0};
    assert(seek_asset(asset, 3, SEEK_SET) == 3);
    assert(read_asset(asset, bytes, 2) == 2 && !memcmp(bytes, "34", 2));
    assert(seek_asset(asset, -3, SEEK_CUR) == 2);
    assert(read_asset(asset, bytes, 2) == 2 && !memcmp(bytes, "23", 2));
    assert(seek_asset(asset, -1, SEEK_SET) == -1);
    assert(seek64(asset, 0, SEEK_CUR) == 4);
    assert(seek_asset(asset, -1, SEEK_END) == tail);
    assert(read_asset(asset, bytes, 1) == 1 && bytes[0] == 'z');
    assert(read_asset(asset, bytes, 1) == 0);
    assert(seek_asset(asset, tail, SEEK_SET) == tail);
    assert(seek64(asset, -tail, SEEK_CUR) == 0);
    assert(read_asset(asset, bytes, 2) == 2 && !memcmp(bytes, "01", 2));
    assert(seek_asset(NULL, 0, SEEK_SET) == -1);
    close_asset(asset);
    assert(!unlink(path));
}
static void expect_directory_eof(void *directory) {
    void *(*next)(void *) = kl_shim_lookup("readdir");
    int *(*error_location)(void) = kl_shim_lookup("__errno");
    int (*close_directory)(void *) = kl_shim_lookup("closedir");
    assert(directory && next && error_location && close_directory);
    errno = EOPNOTSUPP;
    assert(*error_location() == 95);
    *error_location() = 0;
    unsigned count = 0;
    while (next(directory)) assert(++count < 1000);
    // IL2CPP sets guest errno=0 to distinguish EOF from an invalid stream.
    assert(*error_location() == 0);
    errno = EOPNOTSUPP;
    *error_location() = EACCES;
    assert(!next(directory) && *error_location() == EACCES);
    assert(!close_directory(directory));
}
static void expect_directory_error(const char *path) {
    void *(*open_directory)(const char *) = kl_shim_lookup("opendir");
    void *(*next)(void *) = kl_shim_lookup("readdir");
    int *(*error_location)(void) = kl_shim_lookup("__errno");
    int (*close_directory)(void *) = kl_shim_lookup("closedir");
    assert(open_directory && next && error_location && close_directory);
    // No OBB index is initialized yet, so this is a native DIR handle.
    expect_directory_eof(open_directory(path));
    void *directory = open_directory(path); assert(directory);
    assert(!close(dirfd((DIR *)directory)));
    errno = EOPNOTSUPP;
    *error_location() = 0;
    assert(!next(directory) && *error_location() == EBADF);
    close_directory(directory);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    char assets[1024]; long version = 0; const char *version_name = NULL;
    snprintf(assets, sizeof assets, "%s/assets", argv[1]);
    kl_jni_set_assets_dir(assets);
    char mapped[2048], expected[2048];
    snprintf(expected, sizeof expected, "%s/Master Bank.strings.bank", assets);
    assert(!strcmp(kl_guest_path("file:///android_asset/Master Bank.strings.bank", mapped, sizeof mapped), expected));
    assert(!strcmp(kl_guest_path("/android_asset/Master Bank.strings.bank", mapped, sizeof mapped), expected));
    assert(!strcmp(kl_guest_path("file:///android_asset/../private", mapped, sizeof mapped), "file:///android_asset/../private"));

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
    snprintf(expected, sizeof expected, "%s/Master Bank.strings.bank", assets);
    assert(!strcmp(kl_guest_path("file:///android_asset/Master Bank.strings.bank", mapped, sizeof mapped), expected));

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
    expect_directory_error(argv[1]);
    kl_obbmap_init(kl_jni_obb_dir(), "");
    expect_asset("jar:file:///game.apk!/assets/aa/settings.json", "{\"fixture\":true}");
    expect_asset("/game.apk/assets/bin/Data/resource", "packaged settings fixture");
    expect_asset("OvrAvatar2Assets.zip", "avatar core fixture");
    expect_asset("/sdcard/Android/obb/test.game/PresetAvatars_Quest.zip", "avatar presets fixture");
    expect_asset("/sdcard/Android/obb/test.game/PresetAvatars_Quest_Light.zip", "avatar light fixture");
    assert(!kl_obbmap_stat("PresetAvatars_Rift.zip",NULL));
    assert(!kl_obbmap_fopen("PresetAvatars_Rift.zip","rb"));
    assert(!kl_obbmap_fopen("OvrAvatar2Assets.zip","wb"));
    assert(!kl_obbmap_stat("jar:file:///game.apk!/assets/aa/absent.json", NULL));
    void *directory = kl_obbmap_opendir("/game.apk/assets/bin/Data/UnitySubsystems/");
    assert(directory);
    char name[256]; int is_directory = 0;
    assert(kl_obbmap_readdir(directory, name, sizeof name, &is_directory) == 1);
    assert(!strcmp(name, "UnityOpenXR") && is_directory);
    assert(kl_obbmap_readdir(directory, name, sizeof name, &is_directory) == 0);
    expect_directory_eof(directory);
    expect_ndk_asset_seek(argv[2]);
    puts("Steam-to-game path handoff and stored/deflated OBB asset reads passed");
}
