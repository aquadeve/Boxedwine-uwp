/*
 * Boxedwine Android - JNI Environment
 *
 * Provides a stub JNI environment (JNIEnv) and JavaVM that Android native
 * libraries can call through their JNI interface table.
 *
 * The emulator intercepts JNI calls made by native code via the ARMv7 CPU
 * and dispatches them to host implementations here.
 *
 * Based on:
 *   referenceCode/apkenv/jni/jnienv.h  (Thomas Perl)
 *   referenceCode/apkenv/jni/jni.h     (Android Open Source Project)
 *   referenceCode/Bridge/BridgeLib/     (FLinux Android bridge)
 */

#ifndef __ANDROID_JNI_H__
#define __ANDROID_JNI_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Basic JNI primitive types (matching Android's jni.h)
 * ---------------------------------------------------------------------- */
typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

typedef void*    jobject;
typedef jobject  jclass;
typedef jobject  jstring;
typedef jobject  jarray;
typedef jarray   jobjectArray;
typedef jarray   jbooleanArray;
typedef jarray   jbyteArray;
typedef jarray   jcharArray;
typedef jarray   jshortArray;
typedef jarray   jintArray;
typedef jarray   jlongArray;
typedef jarray   jfloatArray;
typedef jarray   jdoubleArray;
typedef jobject  jthrowable;
typedef jobject  jweak;

typedef struct { const char *name; const char *sig; jclass clazz; } _jmethodID;
typedef struct { const char *name; const char *sig; jclass clazz; } _jfieldID;
typedef _jmethodID* jmethodID;
typedef _jfieldID*  jfieldID;

typedef union {
    jboolean z; jbyte b; jchar c; jshort s;
    jint i; jlong j; jfloat f; jdouble d; jobject l;
} jvalue;

#define JNI_FALSE 0
#define JNI_TRUE  1
#define JNI_OK    0
#define JNI_ERR  -1
#define JNI_VERSION_1_4 0x00010004
#define JNI_VERSION_1_6 0x00010006

/* -------------------------------------------------------------------------
 * Dummy object wrappers (host-side representation)
 * ---------------------------------------------------------------------- */
typedef struct { char *data; }         BwJString;
typedef struct { void *data; size_t element_size; int32_t length; } BwJArray;
typedef struct { char *name; }         BwJClass;

/* -------------------------------------------------------------------------
 * JNINativeMethod
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *name;
    const char *signature;
    void       *fnPtr;  /* pointer to the native function in emulated space */
} JNINativeMethod;

/* -------------------------------------------------------------------------
 * Forward declarations for the interface tables
 * ---------------------------------------------------------------------- */
struct JNINativeInterface_;
struct JNIInvokeInterface_;
typedef const struct JNINativeInterface_ *JNIEnv;
typedef const struct JNIInvokeInterface_ *JavaVM;

/* -------------------------------------------------------------------------
 * AndroidJniContext - our host-side state for one JNI session
 * ---------------------------------------------------------------------- */
typedef struct {
    JavaVM      java_vm;
    JNIEnv      jni_env;

    /* Registered native methods: symbol name -> emulated VA */
    struct { char name[256]; uint32_t va; } native_methods[1024];
    unsigned native_method_count;

    /* String pool */
    BwJString  strings[4096];
    unsigned   string_count;

    /* Class pool */
    BwJClass   classes[256];
    unsigned   class_count;
} AndroidJniContext;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * Initialise a JNI context.  Must be called before passing JNIEnv or JavaVM
 * to any native library.
 */
void android_jni_init(AndroidJniContext *ctx);

/**
 * Register a native method (called when the native lib calls
 * JNIEnv->RegisterNatives).
 */
void android_jni_register_native(AndroidJniContext *ctx, const char *name, uint32_t emulated_va);

/**
 * Look up a registered native method by name.
 * Returns the emulated VA, or 0 if not found.
 */
uint32_t android_jni_find_native(const AndroidJniContext *ctx, const char *name);

/**
 * Create a JNI string object wrapping a C string.
 */
jstring android_jni_new_string(AndroidJniContext *ctx, const char *str);

/**
 * Get the C string from a JNI string object.
 */
const char *android_jni_get_string(const AndroidJniContext *ctx, jstring s);

/**
 * Free all resources held by the JNI context.
 */
void android_jni_destroy(AndroidJniContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID_JNI_H__ */
