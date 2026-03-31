/*
 * Boxedwine Android - JNI Environment Implementation
 *
 * Stub JNI interface table (JNIEnv) backed by our host-side AndroidJniContext.
 * Native Android libraries call through the JNIEnv function pointers; each
 * stub here implements the minimum needed to bootstrap native-activity apps.
 *
 * Reference: referenceCode/apkenv/jni/jnienv.c  (Thomas Perl)
 */

#include "android_jni.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Helper to get the host context from a JNIEnv pointer
 * JNINativeInterface_::reserved0 stores the AndroidJniContext*.
 * ---------------------------------------------------------------------- */
#define JNI_CTX(env) ((AndroidJniContext*)(*(const struct JNINativeInterface_**)(env))->reserved0)

/* -------------------------------------------------------------------------
 * JNINativeInterface_ stubs
 * ---------------------------------------------------------------------- */

static jint bw_GetVersion(JNIEnv *env) {
    (void)env;
    return JNI_VERSION_1_6;
}

static jclass bw_FindClass(JNIEnv *env, const char *name) {
    AndroidJniContext *ctx = JNI_CTX(env);
    if (ctx->class_count < 256) {
        BwJClass *cl = &ctx->classes[ctx->class_count++];
        cl->name = strdup(name);
        return (jclass)cl;
    }
    return NULL;
}

static jmethodID bw_GetMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    (void)env; (void)clazz;
    _jmethodID *mid = (struct _jmethodID*)malloc(sizeof(_jmethodID));
    mid->name  = name;
    mid->sig   = sig;
    mid->clazz = clazz;
    return mid;
}

static jmethodID bw_GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    return bw_GetMethodID(env, clazz, name, sig);
}

static jstring bw_NewStringUTF(JNIEnv *env, const char *utf) {
    return android_jni_new_string(JNI_CTX(env), utf);
}

static const char *bw_GetStringUTFChars(JNIEnv *env, jstring str, jboolean *isCopy) {
    if (isCopy) *isCopy = JNI_FALSE;
    return android_jni_get_string(JNI_CTX(env), str);
}

static void bw_ReleaseStringUTFChars(JNIEnv *env, jstring str, const char *chars) {
    (void)env; (void)str; (void)chars;
}

static jsize bw_GetStringUTFLength(JNIEnv *env, jstring str) {
    const char *s = android_jni_get_string(JNI_CTX(env), str);
    return s ? (jsize)strlen(s) : 0;
}

static jint bw_RegisterNatives(JNIEnv *env, jclass clazz,
                                const JNINativeMethod *methods, jint n_methods) {
    AndroidJniContext *ctx = JNI_CTX(env);
    (void)clazz;
    for (jint i = 0; i < n_methods; i++) {
        android_jni_register_native(ctx, methods[i].name, (uint32_t)(uintptr_t)methods[i].fnPtr);
    }
    return JNI_OK;
}

static jthrowable bw_ExceptionOccurred(JNIEnv *env) { (void)env; return NULL; }
static void bw_ExceptionClear(JNIEnv *env)           { (void)env; }
static void bw_ExceptionDescribe(JNIEnv *env)        { (void)env; }
static void bw_FatalError(JNIEnv *env, const char *msg) {
    fprintf(stderr, "JNI FatalError: %s\n", msg);
    (void)env;
}

static jobject bw_NewGlobalRef(JNIEnv *env, jobject obj) { (void)env; return obj; }
static void    bw_DeleteGlobalRef(JNIEnv *env, jobject obj) { (void)env; (void)obj; }
static void    bw_DeleteLocalRef (JNIEnv *env, jobject obj) { (void)env; (void)obj; }
static jboolean bw_IsSameObject(JNIEnv *env, jobject a, jobject b) { (void)env; return a == b; }

static jbyteArray bw_NewByteArray(JNIEnv *env, jsize len) {
    (void)env;
    BwJArray *arr = (BwJArray*)malloc(sizeof(BwJArray));
    arr->data = calloc(len, 1);
    arr->element_size = 1;
    arr->length = len;
    return (jbyteArray)arr;
}

static jbyte *bw_GetByteArrayElements(JNIEnv *env, jbyteArray arr, jboolean *isCopy) {
    (void)env;
    if (isCopy) *isCopy = JNI_FALSE;
    return (jbyte*)((BwJArray*)arr)->data;
}

static void bw_ReleaseByteArrayElements(JNIEnv *env, jbyteArray arr, jbyte *elems, jint mode) {
    (void)env; (void)arr; (void)elems; (void)mode;
}

static jsize bw_GetArrayLength(JNIEnv *env, jarray arr) {
    (void)env;
    return (jsize)((BwJArray*)arr)->length;
}

static jint bw_Throw(JNIEnv *env, jthrowable obj)             { (void)env; (void)obj; return 0; }
static jint bw_ThrowNew(JNIEnv *env, jclass c, const char *m) { (void)env; (void)c; (void)m; return 0; }
static jboolean bw_IsInstanceOf(JNIEnv *env, jobject o, jclass c) { (void)env; (void)o; (void)c; return JNI_TRUE; }
static jclass bw_GetObjectClass(JNIEnv *env, jobject o) { (void)env; (void)o; return NULL; }

static jfieldID bw_GetFieldID(JNIEnv *env, jclass c, const char *n, const char *s) {
    (void)env; (void)c;
    _jfieldID *fid = (struct _jfieldID*)malloc(sizeof(_jfieldID));
    fid->name = n; fid->sig = s; fid->clazz = c;
    return fid;
}
static jfieldID bw_GetStaticFieldID(JNIEnv *env, jclass c, const char *n, const char *s) {
    return bw_GetFieldID(env, c, n, s);
}

static jint    bw_GetIntField(JNIEnv *env, jobject o, jfieldID f)    { (void)env; (void)o; (void)f; return 0; }
static jlong   bw_GetLongField(JNIEnv *env, jobject o, jfieldID f)   { (void)env; (void)o; (void)f; return 0; }
static jfloat  bw_GetFloatField(JNIEnv *env, jobject o, jfieldID f)  { (void)env; (void)o; (void)f; return 0.0f; }
static jdouble bw_GetDoubleField(JNIEnv *env, jobject o, jfieldID f) { (void)env; (void)o; (void)f; return 0.0; }
static jobject bw_GetObjectField(JNIEnv *env, jobject o, jfieldID f) { (void)env; (void)o; (void)f; return NULL; }

static void bw_SetIntField(JNIEnv *env, jobject o, jfieldID f, jint v)    { (void)env; (void)o; (void)f; (void)v; }
static void bw_SetLongField(JNIEnv *env, jobject o, jfieldID f, jlong v)  { (void)env; (void)o; (void)f; (void)v; }
static void bw_SetFloatField(JNIEnv *env, jobject o, jfieldID f, jfloat v){ (void)env; (void)o; (void)f; (void)v; }
static void bw_SetObjectField(JNIEnv *env, jobject o, jfieldID f, jobject v){(void)env;(void)o;(void)f;(void)v;}

static jint bw_PushLocalFrame(JNIEnv *env, jint cap) { (void)env; (void)cap; return 0; }
static jobject bw_PopLocalFrame(JNIEnv *env, jobject res) { (void)env; return res; }

/* Stub call methods */
static jobject bw_CallObjectMethod(JNIEnv *e, jobject o, jmethodID m, ...) {(void)e;(void)o;(void)m; return NULL;}
static jboolean bw_CallBooleanMethod(JNIEnv *e, jobject o, jmethodID m, ...) {(void)e;(void)o;(void)m; return JNI_FALSE;}
static jint bw_CallIntMethod(JNIEnv *e, jobject o, jmethodID m, ...) {(void)e;(void)o;(void)m; return 0;}
static jvoid_t_not_real bw_CallVoidMethod; /* placeholder - see below */

/* We can't define a function returning void with ... easily as a pointer; use macro */
static void bw_CallVoidMethodV(JNIEnv *e, jobject o, jmethodID m, va_list args) {(void)e;(void)o;(void)m;(void)args;}
static void bw_CallVoidMethodA(JNIEnv *e, jobject o, jmethodID m, jvalue *args) {(void)e;(void)o;(void)m;(void)args;}

/* -------------------------------------------------------------------------
 * Build the JNINativeInterface table
 * (Only the entries we use are non-NULL; the rest are NULL stubs)
 * ---------------------------------------------------------------------- */

/* Number of entries in a full JNINativeInterface_ table is 232 */
#define TABLE_SIZE 232

/* We store the AndroidJniContext* in reserved0 */
struct JNINativeInterface_ {
    void *reserved0;
    void *reserved1;
    void *reserved2;
    void *reserved3;

    jint    (*GetVersion)(JNIEnv*);
    jclass  (*DefineClass)(JNIEnv*, const char*, jobject, const jbyte*, jsize);
    jclass  (*FindClass)(JNIEnv*, const char*);
    jmethodID (*FromReflectedMethod)(JNIEnv*, jobject);
    jfieldID  (*FromReflectedField)(JNIEnv*, jobject);
    jobject   (*ToReflectedMethod)(JNIEnv*, jclass, jmethodID, jboolean);
    jclass  (*GetSuperclass)(JNIEnv*, jclass);
    jboolean (*IsAssignableFrom)(JNIEnv*, jclass, jclass);
    jobject (*ToReflectedField)(JNIEnv*, jclass, jfieldID, jboolean);
    jint    (*Throw)(JNIEnv*, jthrowable);
    jint    (*ThrowNew)(JNIEnv*, jclass, const char*);
    jthrowable (*ExceptionOccurred)(JNIEnv*);
    void    (*ExceptionDescribe)(JNIEnv*);
    void    (*ExceptionClear)(JNIEnv*);
    void    (*FatalError)(JNIEnv*, const char*);
    jint    (*PushLocalFrame)(JNIEnv*, jint);
    jobject (*PopLocalFrame)(JNIEnv*, jobject);
    jobject (*NewGlobalRef)(JNIEnv*, jobject);
    void    (*DeleteGlobalRef)(JNIEnv*, jobject);
    void    (*DeleteLocalRef)(JNIEnv*, jobject);
    jboolean (*IsSameObject)(JNIEnv*, jobject, jobject);
    jobject (*NewLocalRef)(JNIEnv*, jobject);
    jint    (*EnsureLocalCapacity)(JNIEnv*, jint);
    /* ... many more - pad with NULLs ... */
    /* We only fill in what we need and zero the rest via the struct initialiser */
};

/* -------------------------------------------------------------------------
 * android_jni_init
 * ---------------------------------------------------------------------- */

void android_jni_init(AndroidJniContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    /* Allocate the interface table on the heap so pointers remain stable */
    struct JNINativeInterface_ *iface =
        (struct JNINativeInterface_*)calloc(1, sizeof(struct JNINativeInterface_));

    iface->reserved0           = ctx;
    iface->GetVersion          = bw_GetVersion;
    iface->FindClass           = bw_FindClass;
    iface->Throw               = bw_Throw;
    iface->ThrowNew            = bw_ThrowNew;
    iface->ExceptionOccurred   = bw_ExceptionOccurred;
    iface->ExceptionDescribe   = bw_ExceptionDescribe;
    iface->ExceptionClear      = bw_ExceptionClear;
    iface->FatalError          = bw_FatalError;
    iface->PushLocalFrame      = bw_PushLocalFrame;
    iface->PopLocalFrame       = bw_PopLocalFrame;
    iface->NewGlobalRef        = bw_NewGlobalRef;
    iface->DeleteGlobalRef     = bw_DeleteGlobalRef;
    iface->DeleteLocalRef      = bw_DeleteLocalRef;
    iface->IsSameObject        = bw_IsSameObject;
    iface->GetMethodID         = bw_GetMethodID;
    iface->GetStaticMethodID   = bw_GetStaticMethodID;
    iface->CallObjectMethod    = bw_CallObjectMethod;
    iface->CallBooleanMethod   = bw_CallBooleanMethod;
    iface->CallIntMethod       = bw_CallIntMethod;
    iface->GetObjectClass      = bw_GetObjectClass;
    iface->IsInstanceOf        = bw_IsInstanceOf;
    iface->GetFieldID          = bw_GetFieldID;
    iface->GetStaticFieldID    = bw_GetStaticFieldID;
    iface->GetObjectField      = bw_GetObjectField;
    iface->GetIntField         = bw_GetIntField;
    iface->GetLongField        = bw_GetLongField;
    iface->GetFloatField       = bw_GetFloatField;
    iface->GetDoubleField      = bw_GetDoubleField;
    iface->SetIntField         = bw_SetIntField;
    iface->SetLongField        = bw_SetLongField;
    iface->SetFloatField       = bw_SetFloatField;
    iface->SetObjectField      = bw_SetObjectField;
    iface->NewStringUTF        = bw_NewStringUTF;
    iface->GetStringUTFLength  = bw_GetStringUTFLength;
    iface->GetStringUTFChars   = bw_GetStringUTFChars;
    iface->ReleaseStringUTFChars = bw_ReleaseStringUTFChars;
    iface->GetArrayLength      = bw_GetArrayLength;
    iface->NewByteArray        = bw_NewByteArray;
    iface->GetByteArrayElements= bw_GetByteArrayElements;
    iface->ReleaseByteArrayElements = bw_ReleaseByteArrayElements;
    iface->RegisterNatives     = (jint (*)(JNIEnv*, jclass, const JNINativeMethod*, jint))bw_RegisterNatives;

    ctx->jni_env = (JNIEnv)iface;
}

/* -------------------------------------------------------------------------
 * android_jni_register_native / find_native
 * ---------------------------------------------------------------------- */

void android_jni_register_native(AndroidJniContext *ctx, const char *name, uint32_t va) {
    if (ctx->native_method_count >= 1024) return;
    strncpy(ctx->native_methods[ctx->native_method_count].name, name, 255);
    ctx->native_methods[ctx->native_method_count].va = va;
    ctx->native_method_count++;
}

uint32_t android_jni_find_native(const AndroidJniContext *ctx, const char *name) {
    for (unsigned i = 0; i < ctx->native_method_count; i++) {
        if (strcmp(ctx->native_methods[i].name, name) == 0)
            return ctx->native_methods[i].va;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * android_jni_new_string / get_string
 * ---------------------------------------------------------------------- */

jstring android_jni_new_string(AndroidJniContext *ctx, const char *str) {
    if (ctx->string_count >= 4096) return NULL;
    BwJString *s = &ctx->strings[ctx->string_count++];
    s->data = strdup(str ? str : "");
    return (jstring)s;
}

const char *android_jni_get_string(const AndroidJniContext *ctx, jstring s) {
    if (!s) return "";
    /* Check if it's one of ours */
    const BwJString *bs = (const BwJString*)s;
    return bs->data ? bs->data : "";
}

/* -------------------------------------------------------------------------
 * android_jni_destroy
 * ---------------------------------------------------------------------- */

void android_jni_destroy(AndroidJniContext *ctx) {
    for (unsigned i = 0; i < ctx->string_count; i++) free(ctx->strings[i].data);
    for (unsigned i = 0; i < ctx->class_count;  i++) free(ctx->classes[i].name);
    /* Free the interface table */
    if (ctx->jni_env) free((void*)*(const struct JNINativeInterface_**)(void*)&ctx->jni_env);
    memset(ctx, 0, sizeof(*ctx));
}
