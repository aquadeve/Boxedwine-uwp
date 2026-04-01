/*
 * Boxedwine Android - APK Loader Implementation
 *
 * Reads an Android APK (ZIP archive) and extracts:
 *   - Native shared libraries (lib/arm64-v8a/(name).so, lib/armeabi-v7a/(name).so, etc.)
 *   - AndroidManifest.xml (binary XML)
 *   - Assets on demand
 *
 * Uses the minizip library already present in lib/zlib/contrib/minizip/.
 *
 * Based on apkenv by Thomas Perl (referenceCode/apkenv)
 */

#include "apk_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Minizip is in lib/zlib/contrib/minizip/ */
#include "../../lib/zlib/contrib/minizip/unzip.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static bool zip_read_entry(unzFile zip, uint8_t **out, size_t *out_size) {
    unz_file_info fi;
    if (unzGetCurrentFileInfo(zip, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK)
        return false;

    *out_size = (size_t)fi.uncompressed_size;
    *out = (uint8_t*)malloc(*out_size);
    if (!*out) return false;

    if (unzOpenCurrentFile(zip) != UNZ_OK) { free(*out); return false; }
    int rd = unzReadCurrentFile(zip, *out, (unsigned)*out_size);
    unzCloseCurrentFile(zip);
    if (rd < 0 || (size_t)rd != *out_size) { free(*out); return false; }
    return true;
}

/* Check whether a path in the ZIP belongs to our target ABI */
static bool path_matches_abi(const char *path, const char *abi) {
    /* path looks like "lib/armeabi-v7a/libfoo.so" */
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "lib/%s/", abi);
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static const char *basename_of(const char *path) {
    const char *sep = strrchr(path, '/');
    return sep ? sep + 1 : path;
}

/* -------------------------------------------------------------------------
 * apk_open
 * ---------------------------------------------------------------------- */

bool apk_open(const char *path, ApkDescriptor *desc) {
    memset(desc, 0, sizeof(*desc));
    snprintf(desc->apk_path, APK_MAX_PATH, "%s", path);

    unzFile zip = unzOpen(path);
    if (!zip) return false;
    desc->zip_handle = zip;

    /* ABI preference order: arm64-v8a > armeabi-v7a > armeabi > x86 */
    const char *abi_list[] = { "arm64-v8a", "armeabi-v7a", "armeabi", "x86", NULL };
    const char *chosen_abi = NULL;

    /* First pass: find which ABI is present */
    for (int ai = 0; abi_list[ai] && !chosen_abi; ai++) {
        char probe[128];
        snprintf(probe, sizeof(probe), "lib/%s/", abi_list[ai]);
        if (unzLocateFile(zip, probe, 0) == UNZ_OK ||
            /* Some APKs don't have directory entries – probe for any .so */
            true) {
            /* Walk all entries looking for this ABI */
            int ret = unzGoToFirstFile(zip);
            while (ret == UNZ_OK) {
                char name[APK_MAX_PATH];
                unzGetCurrentFileInfo(zip, NULL, name, APK_MAX_PATH, NULL, 0, NULL, 0);
                if (path_matches_abi(name, abi_list[ai]) &&
                    strcmp(name + strlen(name) - 3, ".so") == 0) {
                    chosen_abi = abi_list[ai];
                    break;
                }
                ret = unzGoToNextFile(zip);
            }
        }
    }

    if (!chosen_abi) chosen_abi = "armeabi-v7a"; /* default / empty */
    snprintf(desc->target_abi, sizeof(desc->target_abi), "%s", chosen_abi);

    /* Second pass: extract native libraries and manifest */
    int ret = unzGoToFirstFile(zip);
    while (ret == UNZ_OK) {
        char entry_name[APK_MAX_PATH];
        unzGetCurrentFileInfo(zip, NULL, entry_name, APK_MAX_PATH, NULL, 0, NULL, 0);

        /* Native library? */
        if (path_matches_abi(entry_name, chosen_abi) &&
            strlen(entry_name) > 3 &&
            strcmp(entry_name + strlen(entry_name) - 3, ".so") == 0 &&
            desc->lib_count < APK_MAX_LIBS) {

            ApkLibEntry *e = &desc->libs[desc->lib_count];
            snprintf(e->name, APK_MAX_PATH, "%s", basename_of(entry_name));
            if (zip_read_entry(zip, &e->data, &e->size)) {
                desc->lib_count++;
            }
        }

        /* AndroidManifest.xml */
        if (strcmp(entry_name, "AndroidManifest.xml") == 0 && !desc->manifest_data) {
            zip_read_entry(zip, &desc->manifest_data, &desc->manifest_size);
        }

        ret = unzGoToNextFile(zip);
    }

    /* Try to extract package name from binary manifest (very simplified) */
    if (desc->manifest_data && desc->manifest_size > 8) {
        /* Binary XML chunk header magic: 0x0003 0x0001 */
        /* The package name is stored as a UTF-16 string pool entry.
         * For now we derive a name from the APK filename as fallback. */
        const char *base = basename_of(path);
        snprintf(desc->package_name, APK_MAX_PATH, "%s", base);
        /* Remove .apk extension if present */
        size_t len = strlen(desc->package_name);
        if (len > 4 && strcmp(desc->package_name + len - 4, ".apk") == 0)
            desc->package_name[len - 4] = '\0';
    }

    return desc->lib_count > 0 || desc->manifest_data != NULL;
}

/* -------------------------------------------------------------------------
 * apk_read_asset
 * ---------------------------------------------------------------------- */

bool apk_read_asset(ApkDescriptor *desc, const char *name, uint8_t **out, size_t *size) {
    unzFile zip = (unzFile)desc->zip_handle;
    if (!zip) return false;

    if (unzLocateFile(zip, name, 0) != UNZ_OK) return false;
    return zip_read_entry(zip, out, size);
}

/* -------------------------------------------------------------------------
 * apk_close
 * ---------------------------------------------------------------------- */

void apk_close(ApkDescriptor *desc) {
    if (!desc) return;

    for (unsigned i = 0; i < desc->lib_count; i++) {
        free(desc->libs[i].data);
        desc->libs[i].data = NULL;
    }
    if (desc->manifest_data) {
        free(desc->manifest_data);
        desc->manifest_data = NULL;
    }
    if (desc->assets) {
        for (unsigned i = 0; i < desc->asset_count; i++)
            free(desc->assets[i].data);
        free(desc->assets);
        desc->assets = NULL;
    }
    if (desc->zip_handle) {
        unzClose((unzFile)desc->zip_handle);
        desc->zip_handle = NULL;
    }
}

/* -------------------------------------------------------------------------
 * apk_find_lib
 * ---------------------------------------------------------------------- */

const ApkLibEntry *apk_find_lib(const ApkDescriptor *desc, const char *libname) {
    for (unsigned i = 0; i < desc->lib_count; i++) {
        if (strcmp(desc->libs[i].name, libname) == 0)
            return &desc->libs[i];
    }
    return NULL;
}
