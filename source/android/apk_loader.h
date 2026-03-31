/*
 * Boxedwine Android - APK Loader
 *
 * Loads Android application packages (.apk), which are ZIP archives
 * containing:
 *   - AndroidManifest.xml
 *   - classes.dex (Dalvik bytecode, not used for native-only apps)
 *   - lib/(abi)/(name).so   (native shared libraries)
 *   - assets/          (raw asset files)
 *   - res/             (compiled resources)
 *
 * For native-activity and NDK-only applications we only need the .so
 * libraries from lib/armeabi-v7a/ (or lib/armeabi/).
 *
 * Based on ideas from:
 *   referenceCode/apkenv  (Thomas Perl's apkenv)
 *   referenceCode/Bridge  (FLinux / UWP Android bridge)
 */

#ifndef __APK_LOADER_H__
#define __APK_LOADER_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of native libraries in one APK */
#define APK_MAX_LIBS 64

/* Maximum path length */
#define APK_MAX_PATH 512

/* -------------------------------------------------------------------------
 * APK in-memory library descriptor
 * ---------------------------------------------------------------------- */
typedef struct {
    char     name[APK_MAX_PATH];   /* Library name, e.g. "libmain.so"   */
    uint8_t *data;                  /* Mapped/loaded ELF image            */
    size_t   size;                  /* Size of the image in bytes         */
} ApkLibEntry;

/* -------------------------------------------------------------------------
 * APK asset descriptor
 * ---------------------------------------------------------------------- */
typedef struct {
    char     path[APK_MAX_PATH];   /* Path inside APK, e.g. "assets/font.ttf" */
    uint8_t *data;
    size_t   size;
} ApkAssetEntry;

/* -------------------------------------------------------------------------
 * Parsed APK descriptor
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Path to the on-disk APK file */
    char apk_path[APK_MAX_PATH];

    /* Package name extracted from AndroidManifest.xml */
    char package_name[APK_MAX_PATH];

    /* Target ABI preference (armeabi-v7a or armeabi) */
    char target_abi[32];

    /* Native libraries */
    ApkLibEntry libs[APK_MAX_LIBS];
    unsigned    lib_count;

    /* Assets (lazy-loaded on demand) */
    ApkAssetEntry *assets;
    unsigned       asset_count;
    unsigned       asset_capacity;

    /* Raw bytes of AndroidManifest.xml (binary XML format) */
    uint8_t *manifest_data;
    size_t   manifest_size;

    /* Opaque handle to the open ZIP archive */
    void    *zip_handle;
} ApkDescriptor;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * Open and parse an APK file.
 * @param path  Filesystem path to the .apk file.
 * @param desc  Output descriptor; caller must call apk_close() when done.
 * @return true on success.
 */
bool apk_open(const char *path, ApkDescriptor *desc);

/**
 * Load the raw bytes for an asset inside the APK.
 * @param desc   Open APK descriptor.
 * @param name   Asset path inside the APK (e.g. "assets/data.bin").
 * @param out    Receives a malloc'd buffer; caller must free().
 * @param size   Receives the size in bytes.
 * @return true if the asset exists.
 */
bool apk_read_asset(ApkDescriptor *desc, const char *name, uint8_t **out, size_t *size);

/**
 * Close the APK and free all resources.
 */
void apk_close(ApkDescriptor *desc);

/**
 * Return the native library entry with the given base name, or NULL.
 */
const ApkLibEntry *apk_find_lib(const ApkDescriptor *desc, const char *libname);

#ifdef __cplusplus
}
#endif

#endif /* __APK_LOADER_H__ */
