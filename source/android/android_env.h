/*
 * Boxedwine Android - Android Environment & System Properties
 *
 * Provides Android system properties (build.prop), environment setup,
 * and emulated /proc and /sys filesystem stubs needed by Android native
 * libraries and the Dalvik/ART runtime.
 *
 * Reference:
 *   referenceCode/Bridge/BridgeLib/android_init.cpp
 *   Android build.prop format
 */

#ifndef __ANDROID_ENV_H__
#define __ANDROID_ENV_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of system properties */
#define ANDROID_PROP_MAX 256

/* Maximum length of a property name or value */
#define ANDROID_PROP_NAME_MAX 64
#define ANDROID_PROP_VALUE_MAX 128

/* -------------------------------------------------------------------------
 * System property store
 * ---------------------------------------------------------------------- */
typedef struct {
    char name[ANDROID_PROP_NAME_MAX];
    char value[ANDROID_PROP_VALUE_MAX];
} AndroidProperty;

typedef struct {
    AndroidProperty props[ANDROID_PROP_MAX];
    unsigned        prop_count;
} AndroidPropertyStore;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * Initialise the property store with Android-standard default properties.
 */
void android_env_init(AndroidPropertyStore *store);

/**
 * Set a system property (creates if new, updates if exists).
 * @return true on success, false if store is full.
 */
bool android_env_set(AndroidPropertyStore *store, const char *name, const char *value);

/**
 * Get a system property.
 * @return The property value, or NULL if not found.
 */
const char *android_env_get(const AndroidPropertyStore *store, const char *name);

/**
 * Dump all properties to stdout (for debugging).
 */
void android_env_dump(const AndroidPropertyStore *store);

/**
 * Populate an in-memory /proc/cpuinfo string for ARM.
 * @param out_buf  Buffer to write to.
 * @param buf_size Size of the buffer.
 */
void android_env_cpuinfo(char *out_buf, size_t buf_size);

/**
 * Populate an in-memory /proc/version string.
 */
void android_env_proc_version(char *out_buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID_ENV_H__ */
