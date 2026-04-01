/*
 * Boxedwine Android - Android Environment & System Properties Implementation
 *
 * Emulates Android's system property service and provides /proc stubs
 * matching an ARMv7 Android device (API level 19 / Android 4.4).
 *
 * Reference:
 *   referenceCode/Bridge/BridgeLib/android_init.cpp
 *   Android system/core/init/property_service.cpp
 */

#include "android_env.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * android_env_set / android_env_get
 * ---------------------------------------------------------------------- */

bool android_env_set(AndroidPropertyStore *store, const char *name, const char *value) {
    /* Update if exists */
    for (unsigned i = 0; i < store->prop_count; i++) {
        if (strncmp(store->props[i].name, name, ANDROID_PROP_NAME_MAX) == 0) {
            strncpy(store->props[i].value, value, ANDROID_PROP_VALUE_MAX - 1);
            store->props[i].value[ANDROID_PROP_VALUE_MAX - 1] = '\0';
            return true;
        }
    }
    /* Insert new */
    if (store->prop_count >= ANDROID_PROP_MAX) return false;
    AndroidProperty *p = &store->props[store->prop_count++];
    strncpy(p->name, name, ANDROID_PROP_NAME_MAX - 1);
    p->name[ANDROID_PROP_NAME_MAX - 1] = '\0';
    strncpy(p->value, value, ANDROID_PROP_VALUE_MAX - 1);
    p->value[ANDROID_PROP_VALUE_MAX - 1] = '\0';
    return true;
}

const char *android_env_get(const AndroidPropertyStore *store, const char *name) {
    for (unsigned i = 0; i < store->prop_count; i++) {
        if (strncmp(store->props[i].name, name, ANDROID_PROP_NAME_MAX) == 0)
            return store->props[i].value;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * android_env_init  - populate default Android system properties
 *
 * Based on referenceCode/Bridge/BridgeLib/android_init.cpp
 * ---------------------------------------------------------------------- */

void android_env_init(AndroidPropertyStore *store) {
    memset(store, 0, sizeof(*store));

    /* Build information - emulate a generic ARMv7 device */
    android_env_set(store, "ro.build.display.id", "KTU84P");
    android_env_set(store, "ro.build.version.incremental", "1");
    android_env_set(store, "ro.build.version.sdk", "19");
    android_env_set(store, "ro.build.version.release", "4.4.4");
    android_env_set(store, "ro.build.version.codename", "REL");
    android_env_set(store, "ro.build.type", "userdebug");
    android_env_set(store, "ro.build.user", "boxedwine");
    android_env_set(store, "ro.build.host", "localhost");

    /* Product / hardware - ARMv7 emulator */
    android_env_set(store, "ro.product.model", "Boxedwine Android");
    android_env_set(store, "ro.product.brand", "generic");
    android_env_set(store, "ro.product.name", "boxedwine");
    android_env_set(store, "ro.product.device", "generic");
    android_env_set(store, "ro.product.board", "goldfish");
    android_env_set(store, "ro.product.manufacturer", "Boxedwine");
    android_env_set(store, "ro.product.locale.language", "en");
    android_env_set(store, "ro.product.locale.region", "US");

    /* CPU ABI - ARMv7 */
    android_env_set(store, "ro.product.cpu.abi", "armeabi-v7a");
    android_env_set(store, "ro.product.cpu.abi2", "armeabi");
    android_env_set(store, "ro.product.cpu.abilist", "armeabi-v7a,armeabi");
    android_env_set(store, "ro.product.cpu.abilist32", "armeabi-v7a,armeabi");
    android_env_set(store, "ro.product.cpu.abilist64", "");

    /* Hardware info */
    android_env_set(store, "ro.hardware", "goldfish");
    android_env_set(store, "ro.board.platform", "generic");
    android_env_set(store, "ro.revision", "0");

    /* Dalvik VM */
    android_env_set(store, "dalvik.vm.heapsize", "256m");
    android_env_set(store, "dalvik.vm.heapstartsize", "8m");
    android_env_set(store, "dalvik.vm.heapgrowthlimit", "96m");
    android_env_set(store, "dalvik.vm.heapmaxfree", "8m");
    android_env_set(store, "dalvik.vm.heapminfree", "512k");
    android_env_set(store, "dalvik.vm.isa.arm.variant", "cortex-a7");
    android_env_set(store, "dalvik.vm.isa.arm.features", "default");
    android_env_set(store, "dalvik.vm.stack-trace-file", "/data/anr/traces.txt");

    /* Boot and system properties */
    android_env_set(store, "ro.secure", "0");
    android_env_set(store, "ro.debuggable", "1");
    android_env_set(store, "ro.allow.mock.location", "1");
    android_env_set(store, "persist.sys.language", "en");
    android_env_set(store, "persist.sys.country", "US");
    android_env_set(store, "persist.sys.timezone", "UTC");
    android_env_set(store, "gsm.version.ril-impl", "android boxedwine");
    android_env_set(store, "net.bt.name", "Android");
    android_env_set(store, "net.change", "net.qtaguid_enabled");
    android_env_set(store, "net.gprs.local-ip", "10.0.2.15");

    /* Display */
    android_env_set(store, "ro.sf.lcd_density", "240");
    android_env_set(store, "qemu.hw.mainkeys", "0");
    android_env_set(store, "qemu.sf.fake_camera", "none");

    /* OpenGL ES */
    android_env_set(store, "ro.opengles.version", "131072"); /* GL_ES 2.0 */

    /* Misc runtime */
    android_env_set(store, "ro.kernel.android.checkjni", "1");
    android_env_set(store, "init.svc.bootanim", "stopped");
}

/* -------------------------------------------------------------------------
 * android_env_dump
 * ---------------------------------------------------------------------- */

void android_env_dump(const AndroidPropertyStore *store) {
    for (unsigned i = 0; i < store->prop_count; i++) {
        printf("[%s]: [%s]\n", store->props[i].name, store->props[i].value);
    }
}

/* -------------------------------------------------------------------------
 * android_env_cpuinfo - emulate /proc/cpuinfo for ARM
 *
 * This matches what the Android emulator (goldfish) reports.
 * ---------------------------------------------------------------------- */

void android_env_cpuinfo(char *out_buf, size_t buf_size) {
    snprintf(out_buf, buf_size,
        "processor\t: 0\n"
        "model name\t: ARMv7 Processor rev 0 (v7l)\n"
        "BogoMIPS\t: 1000.00\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x41\n"
        "CPU architecture: 7\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0xc07\n"
        "CPU revision\t: 0\n"
        "\n"
        "processor\t: 1\n"
        "model name\t: ARMv7 Processor rev 0 (v7l)\n"
        "BogoMIPS\t: 1000.00\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x41\n"
        "CPU architecture: 7\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0xc07\n"
        "CPU revision\t: 0\n"
        "\n"
        "Hardware\t: Goldfish\n"
        "Revision\t: 0000\n"
        "Serial\t\t: 0000000000000000\n");
}

/* -------------------------------------------------------------------------
 * android_env_proc_version
 * ---------------------------------------------------------------------- */

void android_env_proc_version(char *out_buf, size_t buf_size) {
    snprintf(out_buf, buf_size,
        "Linux version 3.18.0-android (boxedwine@localhost) "
        "(gcc version 4.9 (Boxedwine)) "
        "#1 SMP PREEMPT Wed Jan 1 00:00:00 UTC 2025\n");
}
