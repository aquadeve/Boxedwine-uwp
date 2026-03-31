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

/* Forward declaration of the interface struct (defined further down) */
struct JNINativeInterface_;

/* -------------------------------------------------------------------------
 * Helper to get the host context from a JNIEnv pointer.
 *
 * JNIEnv = const struct JNINativeInterface_ *
 * Function param: JNIEnv *env  => const struct JNINativeInterface_ **env
 *
 * We stored the AndroidJniContext* as the first field (reserved0) of the
 * interface table.  Since the struct definition comes later in this file,
 * we cast through void** to access the first pointer slot.
 * ---------------------------------------------------------------------- */
static inline AndroidJniContext *jni_get_ctx(JNIEnv *env) {
    /* env points to a pointer that points to the table.
     * The table starts with reserved0 (a void*) = our context. */
    void *table = (void*)*env;
    void **slots = (void**)table;
    return (AndroidJniContext*)slots[0]; /* reserved0 */
}
#define JNI_CTX(env) jni_get_ctx(env)

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
    _jmethodID *mid = (_jmethodID*)malloc(sizeof(_jmethodID));
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
    _jfieldID *fid = (_jfieldID*)malloc(sizeof(_jfieldID));
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
static void bw_CallVoidMethod(JNIEnv *e, jobject o, jmethodID m, ...) {(void)e;(void)o;(void)m;}
static void bw_CallVoidMethodV(JNIEnv *e, jobject o, jmethodID m, va_list args) {(void)e;(void)o;(void)m;(void)args;}
static void bw_CallVoidMethodA(JNIEnv *e, jobject o, jmethodID m, jvalue *args) {(void)e;(void)o;(void)m;(void)args;}

static jobject bw_CallStaticObjectMethod(JNIEnv *e, jclass c, jmethodID m, ...) {(void)e;(void)c;(void)m; return NULL;}
static jboolean bw_CallStaticBooleanMethod(JNIEnv *e, jclass c, jmethodID m, ...) {(void)e;(void)c;(void)m; return JNI_FALSE;}
static jint bw_CallStaticIntMethod(JNIEnv *e, jclass c, jmethodID m, ...) {(void)e;(void)c;(void)m; return 0;}
static void bw_CallStaticVoidMethod(JNIEnv *e, jclass c, jmethodID m, ...) {(void)e;(void)c;(void)m;}

static jint bw_GetStaticIntField(JNIEnv *e, jclass c, jfieldID f) {(void)e;(void)c;(void)f; return 0;}
static jobject bw_GetStaticObjectField(JNIEnv *e, jclass c, jfieldID f) {(void)e;(void)c;(void)f; return NULL;}
static void bw_SetStaticIntField(JNIEnv *e, jclass c, jfieldID f, jint v) {(void)e;(void)c;(void)f;(void)v;}
static void bw_SetStaticObjectField(JNIEnv *e, jclass c, jfieldID f, jobject v) {(void)e;(void)c;(void)f;(void)v;}

static jstring bw_NewString(JNIEnv *e, const jchar *chars, jsize len) {(void)e;(void)chars;(void)len; return NULL;}
static jsize bw_GetStringLength(JNIEnv *e, jstring s) {(void)e;(void)s; return 0;}
static const jchar* bw_GetStringChars(JNIEnv *e, jstring s, jboolean *isCopy) {(void)e;(void)s;(void)isCopy; return NULL;}
static void bw_ReleaseStringChars(JNIEnv *e, jstring s, const jchar *chars) {(void)e;(void)s;(void)chars;}

static jobjectArray bw_NewObjectArray(JNIEnv *e, jsize len, jclass clazz, jobject init) {(void)e;(void)len;(void)clazz;(void)init; return NULL;}
static jobject bw_GetObjectArrayElement(JNIEnv *e, jobjectArray arr, jsize i) {(void)e;(void)arr;(void)i; return NULL;}
static void bw_SetObjectArrayElement(JNIEnv *e, jobjectArray arr, jsize i, jobject v) {(void)e;(void)arr;(void)i;(void)v;}

static jintArray bw_NewIntArray(JNIEnv *e, jsize len) {(void)e;(void)len; return NULL;}
static jfloatArray bw_NewFloatArray(JNIEnv *e, jsize len) {(void)e;(void)len; return NULL;}

static jint* bw_GetIntArrayElements(JNIEnv *e, jintArray arr, jboolean *isCopy) {(void)e;(void)arr;(void)isCopy; return NULL;}
static void bw_ReleaseIntArrayElements(JNIEnv *e, jintArray arr, jint *elems, jint mode) {(void)e;(void)arr;(void)elems;(void)mode;}
static jfloat* bw_GetFloatArrayElements(JNIEnv *e, jfloatArray arr, jboolean *isCopy) {(void)e;(void)arr;(void)isCopy; return NULL;}
static void bw_ReleaseFloatArrayElements(JNIEnv *e, jfloatArray arr, jfloat *elems, jint mode) {(void)e;(void)arr;(void)elems;(void)mode;}

static void bw_SetByteArrayRegion(JNIEnv *e, jbyteArray arr, jsize start, jsize len, const jbyte *buf) {(void)e;(void)arr;(void)start;(void)len;(void)buf;}
static void bw_SetIntArrayRegion(JNIEnv *e, jintArray arr, jsize start, jsize len, const jint *buf) {(void)e;(void)arr;(void)start;(void)len;(void)buf;}
static void bw_SetFloatArrayRegion(JNIEnv *e, jfloatArray arr, jsize start, jsize len, const jfloat *buf) {(void)e;(void)arr;(void)start;(void)len;(void)buf;}

static jobject bw_NewLocalRef(JNIEnv *e, jobject obj) {(void)e; return obj;}
static jint bw_EnsureLocalCapacity(JNIEnv *e, jint cap) {(void)e;(void)cap; return 0;}
static jobject bw_AllocObject(JNIEnv *e, jclass c) {(void)e;(void)c; return NULL;}
static jobject bw_NewObject(JNIEnv *e, jclass c, jmethodID m, ...) {(void)e;(void)c;(void)m; return NULL;}

static jint bw_GetJavaVM(JNIEnv *e, JavaVM **vm) {(void)e;(void)vm; return JNI_OK;}

/* -------------------------------------------------------------------------
 * Build the JNINativeInterface table
 *
 * This struct mirrors Android's JNINativeInterface (see jni.h).
 * We define all the function-pointer slots that we populate in
 * android_jni_init().  Unused slots remain NULL (via calloc).
 * ---------------------------------------------------------------------- */

/* We store the AndroidJniContext* in reserved0 */
struct JNINativeInterface_ {
    void *reserved0;
    void *reserved1;
    void *reserved2;
    void *reserved3;

    jint     (*GetVersion)(JNIEnv*);                                           /*   4 */
    jclass   (*DefineClass)(JNIEnv*, const char*, jobject, const jbyte*, jsize);/*   5 */
    jclass   (*FindClass)(JNIEnv*, const char*);                               /*   6 */
    jmethodID(*FromReflectedMethod)(JNIEnv*, jobject);                         /*   7 */
    jfieldID (*FromReflectedField)(JNIEnv*, jobject);                          /*   8 */
    jobject  (*ToReflectedMethod)(JNIEnv*, jclass, jmethodID, jboolean);       /*   9 */
    jclass   (*GetSuperclass)(JNIEnv*, jclass);                                /*  10 */
    jboolean (*IsAssignableFrom)(JNIEnv*, jclass, jclass);                     /*  11 */
    jobject  (*ToReflectedField)(JNIEnv*, jclass, jfieldID, jboolean);         /*  12 */
    jint     (*Throw)(JNIEnv*, jthrowable);                                    /*  13 */
    jint     (*ThrowNew)(JNIEnv*, jclass, const char*);                        /*  14 */
    jthrowable(*ExceptionOccurred)(JNIEnv*);                                   /*  15 */
    void     (*ExceptionDescribe)(JNIEnv*);                                    /*  16 */
    void     (*ExceptionClear)(JNIEnv*);                                       /*  17 */
    void     (*FatalError)(JNIEnv*, const char*);                              /*  18 */
    jint     (*PushLocalFrame)(JNIEnv*, jint);                                 /*  19 */
    jobject  (*PopLocalFrame)(JNIEnv*, jobject);                               /*  20 */
    jobject  (*NewGlobalRef)(JNIEnv*, jobject);                                /*  21 */
    void     (*DeleteGlobalRef)(JNIEnv*, jobject);                             /*  22 */
    void     (*DeleteLocalRef)(JNIEnv*, jobject);                              /*  23 */
    jboolean (*IsSameObject)(JNIEnv*, jobject, jobject);                       /*  24 */
    jobject  (*NewLocalRef)(JNIEnv*, jobject);                                 /*  25 */
    jint     (*EnsureLocalCapacity)(JNIEnv*, jint);                            /*  26 */
    jobject  (*AllocObject)(JNIEnv*, jclass);                                  /*  27 */
    jobject  (*NewObject)(JNIEnv*, jclass, jmethodID, ...);                    /*  28 */
    void*    _NewObjectV;                                                      /*  29 */
    void*    _NewObjectA;                                                      /*  30 */
    jclass   (*GetObjectClass)(JNIEnv*, jobject);                              /*  31 */
    jboolean (*IsInstanceOf)(JNIEnv*, jobject, jclass);                        /*  32 */
    jmethodID(*GetMethodID)(JNIEnv*, jclass, const char*, const char*);        /*  33 */
    jobject  (*CallObjectMethod)(JNIEnv*, jobject, jmethodID, ...);            /*  34 */
    void*    _CallObjectMethodV;                                               /*  35 */
    void*    _CallObjectMethodA;                                               /*  36 */
    jboolean (*CallBooleanMethod)(JNIEnv*, jobject, jmethodID, ...);           /*  37 */
    void*    _CallBooleanMethodV;                                              /*  38 */
    void*    _CallBooleanMethodA;                                              /*  39 */
    void*    _CallByteMethod;                                                  /*  40 */
    void*    _CallByteMethodV;                                                 /*  41 */
    void*    _CallByteMethodA;                                                 /*  42 */
    void*    _CallCharMethod;                                                  /*  43 */
    void*    _CallCharMethodV;                                                 /*  44 */
    void*    _CallCharMethodA;                                                 /*  45 */
    void*    _CallShortMethod;                                                 /*  46 */
    void*    _CallShortMethodV;                                                /*  47 */
    void*    _CallShortMethodA;                                                /*  48 */
    jint     (*CallIntMethod)(JNIEnv*, jobject, jmethodID, ...);               /*  49 */
    void*    _CallIntMethodV;                                                  /*  50 */
    void*    _CallIntMethodA;                                                  /*  51 */
    void*    _CallLongMethod;                                                  /*  52 */
    void*    _CallLongMethodV;                                                 /*  53 */
    void*    _CallLongMethodA;                                                 /*  54 */
    void*    _CallFloatMethod;                                                 /*  55 */
    void*    _CallFloatMethodV;                                                /*  56 */
    void*    _CallFloatMethodA;                                                /*  57 */
    void*    _CallDoubleMethod;                                                /*  58 */
    void*    _CallDoubleMethodV;                                               /*  59 */
    void*    _CallDoubleMethodA;                                               /*  60 */
    void     (*CallVoidMethod)(JNIEnv*, jobject, jmethodID, ...);              /*  61 */
    void     (*CallVoidMethodV)(JNIEnv*, jobject, jmethodID, va_list);         /*  62 */
    void     (*CallVoidMethodA)(JNIEnv*, jobject, jmethodID, jvalue*);         /*  63 */
    /* Non-virtual calls 64-96 */
    void*    _nonvirtual[33];
    jfieldID (*GetFieldID)(JNIEnv*, jclass, const char*, const char*);         /*  94 */
    jobject  (*GetObjectField)(JNIEnv*, jobject, jfieldID);                    /*  95 */
    void*    _GetBooleanField;                                                 /*  96 */
    void*    _GetByteField;                                                    /*  97 */
    void*    _GetCharField;                                                    /*  98 */
    void*    _GetShortField;                                                   /*  99 */
    jint     (*GetIntField)(JNIEnv*, jobject, jfieldID);                       /* 100 */
    jlong    (*GetLongField)(JNIEnv*, jobject, jfieldID);                      /* 101 */
    jfloat   (*GetFloatField)(JNIEnv*, jobject, jfieldID);                     /* 102 */
    jdouble  (*GetDoubleField)(JNIEnv*, jobject, jfieldID);                    /* 103 */
    void     (*SetObjectField)(JNIEnv*, jobject, jfieldID, jobject);           /* 104 */
    void*    _SetBooleanField;                                                 /* 105 */
    void*    _SetByteField;                                                    /* 106 */
    void*    _SetCharField;                                                    /* 107 */
    void*    _SetShortField;                                                   /* 108 */
    void     (*SetIntField)(JNIEnv*, jobject, jfieldID, jint);                 /* 109 */
    void     (*SetLongField)(JNIEnv*, jobject, jfieldID, jlong);               /* 110 */
    void     (*SetFloatField)(JNIEnv*, jobject, jfieldID, jfloat);             /* 111 */
    void*    _SetDoubleField;                                                  /* 112 */
    jmethodID(*GetStaticMethodID)(JNIEnv*, jclass, const char*, const char*);  /* 113 */
    jobject  (*CallStaticObjectMethod)(JNIEnv*, jclass, jmethodID, ...);       /* 114 */
    void*    _CallStaticObjectMethodV;                                         /* 115 */
    void*    _CallStaticObjectMethodA;                                         /* 116 */
    jboolean (*CallStaticBooleanMethod)(JNIEnv*, jclass, jmethodID, ...);      /* 117 */
    void*    _rest1[12]; /* 118-129 various static call types */
    void     (*CallStaticVoidMethod)(JNIEnv*, jclass, jmethodID, ...);         /* 141 */
    void*    _CallStaticVoidMethodV;                                           /* 142 */
    void*    _CallStaticVoidMethodA;                                           /* 143 */
    jfieldID (*GetStaticFieldID)(JNIEnv*, jclass, const char*, const char*);   /* 144 */
    jobject  (*GetStaticObjectField)(JNIEnv*, jclass, jfieldID);               /* 145 */
    void*    _GetStaticBooleanField;                                           /* 146 */
    void*    _GetStaticByteField;                                              /* 147 */
    void*    _GetStaticCharField;                                              /* 148 */
    void*    _GetStaticShortField;                                             /* 149 */
    jint     (*GetStaticIntField)(JNIEnv*, jclass, jfieldID);                  /* 150 */
    void*    _rest2[8]; /* 151-158 remaining static get/set */
    jstring  (*NewString)(JNIEnv*, const jchar*, jsize);                       /* 159 */
    jsize    (*GetStringLength)(JNIEnv*, jstring);                             /* 160 */
    const jchar* (*GetStringChars)(JNIEnv*, jstring, jboolean*);               /* 161 */
    void     (*ReleaseStringChars)(JNIEnv*, jstring, const jchar*);            /* 162 */
    jstring  (*NewStringUTF)(JNIEnv*, const char*);                            /* 163 */
    jsize    (*GetStringUTFLength)(JNIEnv*, jstring);                          /* 164 */
    const char* (*GetStringUTFChars)(JNIEnv*, jstring, jboolean*);             /* 165 */
    void     (*ReleaseStringUTFChars)(JNIEnv*, jstring, const char*);          /* 166 */
    jsize    (*GetArrayLength)(JNIEnv*, jarray);                               /* 167 */
    jobjectArray (*NewObjectArray)(JNIEnv*, jsize, jclass, jobject);            /* 168 */
    jobject  (*GetObjectArrayElement)(JNIEnv*, jobjectArray, jsize);            /* 169 */
    void     (*SetObjectArrayElement)(JNIEnv*, jobjectArray, jsize, jobject);   /* 170 */
    void*    _NewBooleanArray;                                                 /* 171 */
    jbyteArray (*NewByteArray)(JNIEnv*, jsize);                                /* 172 */
    void*    _NewCharArray;                                                    /* 173 */
    void*    _NewShortArray;                                                   /* 174 */
    jintArray  (*NewIntArray)(JNIEnv*, jsize);                                 /* 175 */
    void*    _NewLongArray;                                                    /* 176 */
    jfloatArray(*NewFloatArray)(JNIEnv*, jsize);                               /* 177 */
    void*    _NewDoubleArray;                                                  /* 178 */
    void*    _GetBooleanArrayElements;                                         /* 179 */
    jbyte*   (*GetByteArrayElements)(JNIEnv*, jbyteArray, jboolean*);          /* 180 */
    void*    _GetCharArrayElements;                                            /* 181 */
    void*    _GetShortArrayElements;                                           /* 182 */
    jint*    (*GetIntArrayElements)(JNIEnv*, jintArray, jboolean*);            /* 183 */
    void*    _GetLongArrayElements;                                            /* 184 */
    jfloat*  (*GetFloatArrayElements)(JNIEnv*, jfloatArray, jboolean*);        /* 185 */
    void*    _GetDoubleArrayElements;                                          /* 186 */
    void*    _ReleaseBooleanArrayElements;                                     /* 187 */
    void     (*ReleaseByteArrayElements)(JNIEnv*, jbyteArray, jbyte*, jint);   /* 188 */
    void*    _ReleaseCharArrayElements;                                        /* 189 */
    void*    _ReleaseShortArrayElements;                                       /* 190 */
    void     (*ReleaseIntArrayElements)(JNIEnv*, jintArray, jint*, jint);      /* 191 */
    void*    _ReleaseLongArrayElements;                                        /* 192 */
    void     (*ReleaseFloatArrayElements)(JNIEnv*,jfloatArray,jfloat*,jint);   /* 193 */
    void*    _rest3[22]; /* 194-215: Get/Set array region, bulk ops */
    jint     (*RegisterNatives)(JNIEnv*, jclass, const JNINativeMethod*, jint); /* 215 */
    void*    _UnregisterNatives;                                               /* 216 */
    void*    _MonitorEnter;                                                    /* 217 */
    void*    _MonitorExit;                                                     /* 218 */
    jint     (*GetJavaVM)(JNIEnv*, JavaVM**);                                  /* 219 */
    /* remaining entries through 232 stay NULL */
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
    iface->NewLocalRef         = bw_NewLocalRef;
    iface->EnsureLocalCapacity = bw_EnsureLocalCapacity;
    iface->AllocObject         = bw_AllocObject;
    iface->NewObject           = bw_NewObject;
    iface->GetObjectClass      = bw_GetObjectClass;
    iface->IsInstanceOf        = bw_IsInstanceOf;
    iface->GetMethodID         = bw_GetMethodID;
    iface->CallObjectMethod    = bw_CallObjectMethod;
    iface->CallBooleanMethod   = bw_CallBooleanMethod;
    iface->CallIntMethod       = bw_CallIntMethod;
    iface->CallVoidMethod      = bw_CallVoidMethod;
    iface->CallVoidMethodV     = bw_CallVoidMethodV;
    iface->CallVoidMethodA     = bw_CallVoidMethodA;
    iface->GetFieldID          = bw_GetFieldID;
    iface->GetObjectField      = bw_GetObjectField;
    iface->GetIntField         = bw_GetIntField;
    iface->GetLongField        = bw_GetLongField;
    iface->GetFloatField       = bw_GetFloatField;
    iface->GetDoubleField      = bw_GetDoubleField;
    iface->SetObjectField      = bw_SetObjectField;
    iface->SetIntField         = bw_SetIntField;
    iface->SetLongField        = bw_SetLongField;
    iface->SetFloatField       = bw_SetFloatField;
    iface->GetStaticMethodID   = bw_GetStaticMethodID;
    iface->CallStaticObjectMethod  = bw_CallStaticObjectMethod;
    iface->CallStaticBooleanMethod = bw_CallStaticBooleanMethod;
    iface->CallStaticVoidMethod    = bw_CallStaticVoidMethod;
    iface->GetStaticFieldID    = bw_GetStaticFieldID;
    iface->GetStaticObjectField= bw_GetStaticObjectField;
    iface->GetStaticIntField   = bw_GetStaticIntField;
    iface->NewString           = bw_NewString;
    iface->GetStringLength     = bw_GetStringLength;
    iface->GetStringChars      = bw_GetStringChars;
    iface->ReleaseStringChars  = bw_ReleaseStringChars;
    iface->NewStringUTF        = bw_NewStringUTF;
    iface->GetStringUTFLength  = bw_GetStringUTFLength;
    iface->GetStringUTFChars   = bw_GetStringUTFChars;
    iface->ReleaseStringUTFChars = bw_ReleaseStringUTFChars;
    iface->GetArrayLength      = bw_GetArrayLength;
    iface->NewObjectArray      = bw_NewObjectArray;
    iface->GetObjectArrayElement = bw_GetObjectArrayElement;
    iface->SetObjectArrayElement = bw_SetObjectArrayElement;
    iface->NewByteArray        = bw_NewByteArray;
    iface->NewIntArray         = bw_NewIntArray;
    iface->NewFloatArray       = bw_NewFloatArray;
    iface->GetByteArrayElements= bw_GetByteArrayElements;
    iface->GetIntArrayElements = bw_GetIntArrayElements;
    iface->GetFloatArrayElements = bw_GetFloatArrayElements;
    iface->ReleaseByteArrayElements = bw_ReleaseByteArrayElements;
    iface->ReleaseIntArrayElements  = bw_ReleaseIntArrayElements;
    iface->ReleaseFloatArrayElements= bw_ReleaseFloatArrayElements;
    iface->RegisterNatives     = (jint (*)(JNIEnv*, jclass, const JNINativeMethod*, jint))bw_RegisterNatives;
    iface->GetJavaVM           = bw_GetJavaVM;

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
    /* Free the interface table (which was calloc'd in android_jni_init) */
    if (ctx->jni_env) free((void*)ctx->jni_env);
    memset(ctx, 0, sizeof(*ctx));
}
