#include "qjs_jni.h"

CallbackSlot g_callbacks[MAX_CALLBACKS];

JSValue js_java_callback(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    (void)this_val;

    if (magic < 0 || magic >= MAX_CALLBACKS || !g_callbacks[magic].used) {
        return JS_ThrowInternalError(ctx, "callback has been released");
    }

    CallbackSlot *slot = &g_callbacks[magic];

    JNIEnv *env = NULL;
    int attached = 0;
    jint r = (*slot->vm)->GetEnv(slot->vm, (void **)&env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        (*slot->vm)->AttachCurrentThread(slot->vm, (void **)&env, NULL);
        attached = 1;
    }
    if (!env) {
        return JS_ThrowInternalError(ctx, "cannot attach to JVM");
    }

    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jobjectArray jargs = (*env)->NewObjectArray(env, argc, string_class, NULL);

    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (str) {
            jstring jstr = utf8_to_jstring(env, str, len);
            JS_FreeCString(ctx, str);
            if (jstr) {
                (*env)->SetObjectArrayElement(env, jargs, i, jstr);
                (*env)->DeleteLocalRef(env, jstr);
            }
        }
    }

    jstring jresult = (jstring)(*env)->CallObjectMethod(env, slot->callback,
                                                           slot->invoke_method, jargs);

    JSValue jsresult = JS_UNDEFINED;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, jargs);
        (*env)->DeleteLocalRef(env, string_class);
        if (attached)
            (*slot->vm)->DetachCurrentThread(slot->vm);
        return JS_ThrowInternalError(ctx, "Java callback threw an exception");
    }
    if (jresult) {
        Utf8String utf8;
        if (jstring_to_utf8(env, jresult, &utf8)) {
            jsresult = JS_NewStringLen(ctx, utf8.bytes, utf8.len);
            free(utf8.bytes);
        }
        (*env)->DeleteLocalRef(env, jresult);
    }

    (*env)->DeleteLocalRef(env, jargs);
    (*env)->DeleteLocalRef(env, string_class);

    if (attached) {
        (*slot->vm)->DetachCurrentThread(slot->vm);
    }

    return jsresult;
}

int register_callback(JavaVM *vm, jobject callback, jmethodID invoke_method)
{
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (!g_callbacks[i].used) {
            g_callbacks[i].vm = vm;
            g_callbacks[i].callback = callback;
            g_callbacks[i].invoke_method = invoke_method;
            g_callbacks[i].used = true;
            return i;
        }
    }
    return -1;
}

void unregister_callback(JNIEnv *env, int slot)
{
    if (slot >= 0 && slot < MAX_CALLBACKS && g_callbacks[slot].used) {
        (*env)->DeleteGlobalRef(env, g_callbacks[slot].callback);
        g_callbacks[slot].used = false;
        g_callbacks[slot].callback = NULL;
    }
}
