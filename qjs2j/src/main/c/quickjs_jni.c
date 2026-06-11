#include <jni.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "quickjs.h"

#define QUICKJS_CLASS "io/github/quickjsng/QuickJs"
#define QUICKJS_EXCEPTION_CLASS "io/github/quickjsng/QuickJsException"
#define JSFUNCTION_CLASS "io/github/quickjsng/JsFunction"
#define CONSUMER_CLASS "java/util/function/Consumer"
#define countof(x) (sizeof(x) / sizeof((x)[0]))

#define MAX_CALLBACKS 1024

typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
    int64_t deadline_ms;
    JavaVM *vm;
    struct {
        JavaVM *vm;
        jobject handler;
        jmethodID accept_method;
    } console_log;
    struct {
        JavaVM *vm;
        jobject handler;
        jmethodID accept_method;
    } console_error;
} QuickJsNative;

typedef struct {
    char *bytes;
    size_t len;
} Utf8String;

typedef struct {
    JavaVM *vm;
    jobject callback;
    jmethodID invoke_method;
    bool used;
} CallbackSlot;

static CallbackSlot g_callbacks[MAX_CALLBACKS];

static int64_t now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static QuickJsNative *from_handle(JNIEnv *env, jlong handle)
{
    if (handle == 0) {
        jclass cls = (*env)->FindClass(env, "java/lang/IllegalStateException");
        if (cls) {
            (*env)->ThrowNew(env, cls, "QuickJs is closed");
        }
        return NULL;
    }
    return (QuickJsNative *)(intptr_t)handle;
}

static void throw_exception_string(JNIEnv *env, const char *class_name, const char *message)
{
    jclass cls = (*env)->FindClass(env, class_name);
    if (cls) {
        (*env)->ThrowNew(env, cls, message);
    }
}

static uint32_t replacement_codepoint(void)
{
    return 0xfffd;
}

static size_t utf8_len(uint32_t cp)
{
    if (cp <= 0x7f) {
        return 1;
    }
    if (cp <= 0x7ff) {
        return 2;
    }
    if (cp <= 0xffff) {
        return 3;
    }
    return 4;
}

static char *write_utf8(char *out, uint32_t cp)
{
    if (cp <= 0x7f) {
        *out++ = (char)cp;
    } else if (cp <= 0x7ff) {
        *out++ = (char)(0xc0 | (cp >> 6));
        *out++ = (char)(0x80 | (cp & 0x3f));
    } else if (cp <= 0xffff) {
        *out++ = (char)(0xe0 | (cp >> 12));
        *out++ = (char)(0x80 | ((cp >> 6) & 0x3f));
        *out++ = (char)(0x80 | (cp & 0x3f));
    } else {
        *out++ = (char)(0xf0 | (cp >> 18));
        *out++ = (char)(0x80 | ((cp >> 12) & 0x3f));
        *out++ = (char)(0x80 | ((cp >> 6) & 0x3f));
        *out++ = (char)(0x80 | (cp & 0x3f));
    }
    return out;
}

static bool jstring_to_utf8(JNIEnv *env, jstring input, Utf8String *out)
{
    out->bytes = NULL;
    out->len = 0;

    jsize length = (*env)->GetStringLength(env, input);
    const jchar *chars = (*env)->GetStringChars(env, input, NULL);
    if (!chars) {
        return false;
    }

    size_t byte_len = 0;
    for (jsize i = 0; i < length; i++) {
        uint32_t cp = chars[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 1 < length && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
                cp = 0x10000 + (((cp - 0xd800) << 10) | (chars[++i] - 0xdc00));
            } else {
                cp = replacement_codepoint();
            }
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            cp = replacement_codepoint();
        }
        byte_len += utf8_len(cp);
    }

    char *bytes = malloc(byte_len + 1);
    if (!bytes) {
        (*env)->ReleaseStringChars(env, input, chars);
        throw_exception_string(env, "java/lang/OutOfMemoryError", "Unable to allocate UTF-8 string");
        return false;
    }

    char *p = bytes;
    for (jsize i = 0; i < length; i++) {
        uint32_t cp = chars[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 1 < length && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
                cp = 0x10000 + (((cp - 0xd800) << 10) | (chars[++i] - 0xdc00));
            } else {
                cp = replacement_codepoint();
            }
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            cp = replacement_codepoint();
        }
        p = write_utf8(p, cp);
    }
    bytes[byte_len] = '\0';

    (*env)->ReleaseStringChars(env, input, chars);
    out->bytes = bytes;
    out->len = byte_len;
    return true;
}

static uint32_t read_utf8(const unsigned char *bytes, size_t len, size_t *index)
{
    size_t i = *index;
    unsigned char c = bytes[i++];

    if (c < 0x80) {
        *index = i;
        return c;
    }

    uint32_t cp;
    int extra;
    uint32_t min_cp;
    if ((c & 0xe0) == 0xc0) {
        cp = c & 0x1f;
        extra = 1;
        min_cp = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        cp = c & 0x0f;
        extra = 2;
        min_cp = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        cp = c & 0x07;
        extra = 3;
        min_cp = 0x10000;
    } else {
        *index = i;
        return replacement_codepoint();
    }

    if (i + (size_t)extra > len) {
        *index = i;
        return replacement_codepoint();
    }

    for (int j = 0; j < extra; j++) {
        unsigned char cc = bytes[i++];
        if ((cc & 0xc0) != 0x80) {
            *index = i;
            return replacement_codepoint();
        }
        cp = (cp << 6) | (cc & 0x3f);
    }

    if (cp < min_cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        cp = replacement_codepoint();
    }
    *index = i;
    return cp;
}

static jstring utf8_to_jstring(JNIEnv *env, const char *input, size_t len)
{
    jchar *chars = malloc((len + 1) * sizeof(*chars));
    if (!chars) {
        throw_exception_string(env, "java/lang/OutOfMemoryError", "Unable to allocate Java string");
        return NULL;
    }

    size_t i = 0;
    jsize out_len = 0;
    const unsigned char *bytes = (const unsigned char *)input;
    while (i < len) {
        uint32_t cp = read_utf8(bytes, len, &i);
        if (cp <= 0xffff) {
            chars[out_len++] = (jchar)cp;
        } else {
            cp -= 0x10000;
            chars[out_len++] = (jchar)(0xd800 + (cp >> 10));
            chars[out_len++] = (jchar)(0xdc00 + (cp & 0x3ff));
        }
    }

    jstring result = (*env)->NewString(env, chars, out_len);
    free(chars);
    return result;
}

static void throw_quickjs_exception(JNIEnv *env, const char *message, size_t message_len)
{
    jclass cls = (*env)->FindClass(env, QUICKJS_EXCEPTION_CLASS);
    if (!cls) {
        return;
    }

    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(Ljava/lang/String;)V");
    if (!ctor) {
        return;
    }

    jstring jmessage = utf8_to_jstring(env, message, message_len);
    if (!jmessage) {
        return;
    }

    jthrowable throwable = (jthrowable)(*env)->NewObject(env, cls, ctor, jmessage);
    if (throwable) {
        (*env)->Throw(env, throwable);
    }
}

static bool js_value_to_utf8(JSContext *ctx, JSValueConst value, Utf8String *out)
{
    out->bytes = NULL;
    out->len = 0;

    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, value);
    if (!str) {
        return false;
    }

    char *copy = malloc(len + 1);
    if (!copy) {
        JS_FreeCString(ctx, str);
        return false;
    }

    memcpy(copy, str, len);
    copy[len] = '\0';
    JS_FreeCString(ctx, str);
    out->bytes = copy;
    out->len = len;
    return true;
}

static Utf8String js_exception_to_utf8(JSContext *ctx)
{
    Utf8String out = { NULL, 0 };
    JSValue exception = JS_GetException(ctx);
    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");

    if (!JS_IsException(stack) && !JS_IsUndefined(stack) && !JS_IsNull(stack)) {
        if (js_value_to_utf8(ctx, stack, &out)) {
            JS_FreeValue(ctx, stack);
            JS_FreeValue(ctx, exception);
            return out;
        }
    }

    JS_FreeValue(ctx, stack);
    if (!js_value_to_utf8(ctx, exception, &out)) {
        static const char fallback[] = "JavaScript exception";
        out.bytes = malloc(sizeof(fallback));
        if (out.bytes) {
            memcpy(out.bytes, fallback, sizeof(fallback));
            out.len = sizeof(fallback) - 1;
        }
    }
    JS_FreeValue(ctx, exception);
    return out;
}

static void throw_current_js_exception(JNIEnv *env, JSContext *ctx)
{
    Utf8String message = js_exception_to_utf8(ctx);
    if (message.bytes) {
        throw_quickjs_exception(env, message.bytes, message.len);
        free(message.bytes);
    } else {
        throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "JavaScript exception");
    }
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    QuickJsNative *engine = opaque;
    if (engine && engine->deadline_ms > 0 && now_ms() >= engine->deadline_ms) {
        return 1;
    }
    return 0;
}

static void emit_console_message(QuickJsNative *engine, int type,
                                  const char *msg, size_t msg_len)
{
    struct {
        JavaVM *vm;
        jobject *handler_ptr;
        jmethodID *method_ptr;
    } handlers[] = {
        { engine->console_log.vm,    &engine->console_log.handler,    &engine->console_log.accept_method },
        { engine->console_error.vm,  &engine->console_error.handler,  &engine->console_error.accept_method },
    };

    int idx = (type >= 0 && type < 2) ? type : 0;
    if (handlers[idx].vm && *handlers[idx].handler_ptr) {
        JNIEnv *env = NULL;
        int attached = 0;
        jint r = (*handlers[idx].vm)->GetEnv(handlers[idx].vm, (void **)&env, JNI_VERSION_1_6);
        if (r == JNI_EDETACHED) {
            (*handlers[idx].vm)->AttachCurrentThread(handlers[idx].vm, (void **)&env, NULL);
            attached = 1;
        }
        if (env) {
            jstring jmsg = utf8_to_jstring(env, msg, msg_len);
            if (jmsg) {
                (*env)->CallVoidMethod(env, *handlers[idx].handler_ptr, *handlers[idx].method_ptr, jmsg);
                (*env)->DeleteLocalRef(env, jmsg);
            }
            if (attached) {
                (*handlers[idx].vm)->DetachCurrentThread(handlers[idx].vm);
            }
        }
    } else {
        fwrite(msg, 1, msg_len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    QuickJsNative *engine = JS_GetContextOpaque(ctx);

    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s)
            return JS_EXCEPTION;
        total += len;
        if (i > 0)
            total += 1;
        JS_FreeCString(ctx, s);
    }

    char *buf = malloc(total + 1);
    if (!buf)
        return JS_EXCEPTION;

    char *p = buf;
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s) {
            free(buf);
            return JS_EXCEPTION;
        }
        if (i > 0)
            *p++ = ' ';
        memcpy(p, s, len);
        p += len;
        JS_FreeCString(ctx, s);
    }
    *p = '\0';

    if (engine) {
        emit_console_message(engine, 0, buf, total);
    } else {
        fwrite(buf, 1, total, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }

    free(buf);
    return JS_UNDEFINED;
}

static JSValue js_console_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    QuickJsNative *engine = JS_GetContextOpaque(ctx);

    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s)
            return JS_EXCEPTION;
        total += len;
        if (i > 0)
            total += 1;
        JS_FreeCString(ctx, s);
    }

    char *buf = malloc(total + 1);
    if (!buf)
        return JS_EXCEPTION;

    char *p = buf;
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s) {
            free(buf);
            return JS_EXCEPTION;
        }
        if (i > 0)
            *p++ = ' ';
        memcpy(p, s, len);
        p += len;
        JS_FreeCString(ctx, s);
    }
    *p = '\0';

    if (engine) {
        emit_console_message(engine, 1, buf, total);
    } else {
        fwrite(buf, 1, total, stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }

    free(buf);
    return JS_UNDEFINED;
}

static void install_console(JSContext *ctx)
{
    static const JSCFunctionListEntry console_funcs[] = {
        JS_CFUNC_DEF("log", 0, js_console_log),
        JS_CFUNC_DEF("info", 0, js_console_log),
        JS_CFUNC_DEF("warn", 0, js_console_error),
        JS_CFUNC_DEF("error", 0, js_console_error),
        JS_CFUNC_DEF("debug", 0, js_console_log),
    };

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, console, console_funcs, countof(console_funcs));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_console_log, "print", 0));
    JS_FreeValue(ctx, global);
}

static int run_pending_jobs(QuickJsNative *engine, JSContext **exception_ctx)
{
    JSContext *ctx = NULL;
    int result;
    while ((result = JS_ExecutePendingJob(engine->rt, &ctx)) > 0) {
        if (engine->deadline_ms > 0 && now_ms() >= engine->deadline_ms) {
            JS_ThrowInternalError(engine->ctx, "interrupted");
            *exception_ctx = engine->ctx;
            return -1;
        }
    }
    if (result < 0) {
        *exception_ctx = ctx ? ctx : engine->ctx;
        return -1;
    }
    return 0;
}

/* ── callback infrastructure ─────────────────────────────────────── */

static JSValue js_java_callback(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv,
                                 int magic)
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

static int register_callback(JavaVM *vm, jobject callback, jmethodID invoke_method)
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

static void unregister_callback(JNIEnv *env, int slot)
{
    if (slot >= 0 && slot < MAX_CALLBACKS && g_callbacks[slot].used) {
        (*env)->DeleteGlobalRef(env, g_callbacks[slot].callback);
        g_callbacks[slot].used = false;
        g_callbacks[slot].callback = NULL;
    }
}

/* ── native: create / close ──────────────────────────────────────── */

static jlong native_create(JNIEnv *env, jclass cls, jlong memory_limit, jlong stack_size)
{
    (void)cls;

    QuickJsNative *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        throw_exception_string(env, "java/lang/OutOfMemoryError", "Unable to allocate QuickJS handle");
        return 0;
    }

    engine->rt = JS_NewRuntime();
    if (!engine->rt) {
        free(engine);
        throw_exception_string(env, "java/lang/OutOfMemoryError", "Unable to create QuickJS runtime");
        return 0;
    }

    if (memory_limit > 0) {
        JS_SetMemoryLimit(engine->rt, (size_t)memory_limit);
    }
    if (stack_size > 0) {
        JS_SetMaxStackSize(engine->rt, (size_t)stack_size);
    }

    JS_SetInterruptHandler(engine->rt, interrupt_handler, engine);
    engine->ctx = JS_NewContext(engine->rt);
    if (!engine->ctx) {
        JS_FreeRuntime(engine->rt);
        free(engine);
        throw_exception_string(env, "java/lang/OutOfMemoryError", "Unable to create QuickJS context");
        return 0;
    }

    (*env)->GetJavaVM(env, &engine->vm);
    engine->console_log.vm = engine->vm;
    engine->console_error.vm = engine->vm;
    JS_SetContextOpaque(engine->ctx, engine);
    install_console(engine->ctx);
    return (jlong)(intptr_t)engine;
}

static void native_close(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) {
        return;
    }

    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (g_callbacks[i].used && g_callbacks[i].vm == engine->vm) {
            (*env)->DeleteGlobalRef(env, g_callbacks[i].callback);
            g_callbacks[i].used = false;
            g_callbacks[i].callback = NULL;
        }
    }

    if (engine->console_log.handler) {
        (*env)->DeleteGlobalRef(env, engine->console_log.handler);
        engine->console_log.handler = NULL;
    }
    if (engine->console_error.handler) {
        (*env)->DeleteGlobalRef(env, engine->console_error.handler);
        engine->console_error.handler = NULL;
    }

    JS_FreeContext(engine->ctx);
    JS_FreeRuntime(engine->rt);
    free(engine);
}

/* ── native: eval ────────────────────────────────────────────────── */

static jstring eval_common(
    JNIEnv *env,
    jlong handle,
    jstring source,
    jstring filename,
    jboolean module,
    jlong timeout_ms,
    bool as_json)
{
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) {
        return NULL;
    }

    Utf8String source_utf8;
    Utf8String filename_utf8;
    if (!jstring_to_utf8(env, source, &source_utf8)) {
        return NULL;
    }
    if (!jstring_to_utf8(env, filename, &filename_utf8)) {
        free(source_utf8.bytes);
        return NULL;
    }

    int flags = module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    JS_UpdateStackTop(engine->rt);
    engine->deadline_ms = timeout_ms > 0 ? now_ms() + timeout_ms : 0;

    JSValue result = JS_Eval(
        engine->ctx,
        source_utf8.bytes,
        source_utf8.len,
        filename_utf8.bytes,
        flags);

    free(source_utf8.bytes);
    free(filename_utf8.bytes);

    if (JS_IsException(result)) {
        engine->deadline_ms = 0;
        throw_current_js_exception(env, engine->ctx);
        return NULL;
    }

    JSContext *job_exception_ctx = NULL;
    if (run_pending_jobs(engine, &job_exception_ctx) < 0) {
        JS_FreeValue(engine->ctx, result);
        engine->deadline_ms = 0;
        throw_current_js_exception(env, job_exception_ctx);
        return NULL;
    }

    JSValue output = result;
    if (as_json) {
        output = JS_JSONStringify(engine->ctx, result, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(engine->ctx, result);
        if (JS_IsException(output)) {
            engine->deadline_ms = 0;
            throw_current_js_exception(env, engine->ctx);
            return NULL;
        }
        if (JS_IsUndefined(output)) {
            JS_FreeValue(engine->ctx, output);
            engine->deadline_ms = 0;
            return NULL;
        }
    }

    Utf8String output_utf8;
    if (!js_value_to_utf8(engine->ctx, output, &output_utf8)) {
        JS_FreeValue(engine->ctx, output);
        engine->deadline_ms = 0;
        if (JS_HasException(engine->ctx)) {
            throw_current_js_exception(env, engine->ctx);
        } else {
            throw_exception_string(env, "java/lang/OutOfMemoryError", "Unable to allocate result string");
        }
        return NULL;
    }

    JS_FreeValue(engine->ctx, output);
    engine->deadline_ms = 0;

    jstring java_result = utf8_to_jstring(env, output_utf8.bytes, output_utf8.len);
    free(output_utf8.bytes);
    return java_result;
}

static jstring native_eval(
    JNIEnv *env,
    jclass cls,
    jlong handle,
    jstring source,
    jstring filename,
    jboolean module,
    jlong timeout_ms)
{
    (void)cls;
    return eval_common(env, handle, source, filename, module, timeout_ms, false);
}

static jstring native_eval_as_json(
    JNIEnv *env,
    jclass cls,
    jlong handle,
    jstring source,
    jstring filename,
    jboolean module,
    jlong timeout_ms)
{
    (void)cls;
    return eval_common(env, handle, source, filename, module, timeout_ms, true);
}

/* ── native: gc / version ────────────────────────────────────────── */

static void native_gc(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (engine) {
        JS_RunGC(engine->rt);
    }
}

static jstring native_version(JNIEnv *env, jclass cls)
{
    (void)cls;
    const char *version = JS_GetVersion();
    return utf8_to_jstring(env, version, strlen(version));
}

/* ── native: getGlobal / setGlobal ───────────────────────────────── */

static jstring native_get_global(JNIEnv *env, jclass cls, jlong handle, jstring name)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return NULL;

    Utf8String name_utf8;
    if (!jstring_to_utf8(env, name, &name_utf8)) return NULL;

    JS_UpdateStackTop(engine->rt);
    JSValue global = JS_GetGlobalObject(engine->ctx);
    JSValue value = JS_GetPropertyStr(engine->ctx, global, name_utf8.bytes);
    JS_FreeValue(engine->ctx, global);
    free(name_utf8.bytes);

    if (JS_IsUndefined(value) || JS_IsException(value)) {
        JS_FreeValue(engine->ctx, value);
        return NULL;
    }

    JSValue json = JS_JSONStringify(engine->ctx, value, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(engine->ctx, value);

    if (JS_IsException(json) || JS_IsUndefined(json)) {
        JS_FreeValue(engine->ctx, json);
        return NULL;
    }

    Utf8String json_utf8;
    if (!js_value_to_utf8(engine->ctx, json, &json_utf8)) {
        JS_FreeValue(engine->ctx, json);
        return NULL;
    }
    JS_FreeValue(engine->ctx, json);

    jstring result = utf8_to_jstring(env, json_utf8.bytes, json_utf8.len);
    free(json_utf8.bytes);
    return result;
}

static void native_set_global(JNIEnv *env, jclass cls, jlong handle,
                               jstring name, jstring json_value)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return;

    Utf8String name_utf8;
    if (!jstring_to_utf8(env, name, &name_utf8)) return;

    Utf8String value_utf8;
    if (!jstring_to_utf8(env, json_value, &value_utf8)) {
        free(name_utf8.bytes);
        return;
    }

    JS_UpdateStackTop(engine->rt);
    JSValue value = JS_ParseJSON(engine->ctx, value_utf8.bytes, value_utf8.len, "<setGlobal>");
    free(value_utf8.bytes);

    if (JS_IsException(value)) {
        free(name_utf8.bytes);
        throw_current_js_exception(env, engine->ctx);
        return;
    }

    JSValue global = JS_GetGlobalObject(engine->ctx);
    JS_SetPropertyStr(engine->ctx, global, name_utf8.bytes, value);
    JS_FreeValue(engine->ctx, global);
    free(name_utf8.bytes);
}

/* ── native: call ────────────────────────────────────────────────── */

static jstring native_call(JNIEnv *env, jclass cls, jlong handle,
                            jstring func_name, jobjectArray args)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return NULL;

    Utf8String name_utf8;
    if (!jstring_to_utf8(env, func_name, &name_utf8)) return NULL;

    JS_UpdateStackTop(engine->rt);
    JSValue global = JS_GetGlobalObject(engine->ctx);
    JSValue func = JS_GetPropertyStr(engine->ctx, global, name_utf8.bytes);
    free(name_utf8.bytes);

    if (!JS_IsFunction(engine->ctx, func)) {
        JS_FreeValue(engine->ctx, func);
        JS_FreeValue(engine->ctx, global);
        throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "not a function");
        return NULL;
    }

    int argc = args ? (*env)->GetArrayLength(env, args) : 0;
    JSValue *jsargs = argc > 0 ? malloc(argc * sizeof(JSValue)) : NULL;

    for (int i = 0; i < argc; i++) {
        jstring jstr = (jstring)(*env)->GetObjectArrayElement(env, args, i);
        if (jstr) {
            Utf8String arg_utf8;
            if (jstring_to_utf8(env, jstr, &arg_utf8)) {
                jsargs[i] = JS_NewStringLen(engine->ctx, arg_utf8.bytes, arg_utf8.len);
                free(arg_utf8.bytes);
            } else {
                jsargs[i] = JS_UNDEFINED;
            }
            (*env)->DeleteLocalRef(env, jstr);
        } else {
            jsargs[i] = JS_NULL;
        }
    }

    engine->deadline_ms = 0;
    JSValue result = JS_Call(engine->ctx, func, global, argc, jsargs);

    for (int i = 0; i < argc; i++) {
        JS_FreeValue(engine->ctx, jsargs[i]);
    }
    free(jsargs);
    JS_FreeValue(engine->ctx, func);
    JS_FreeValue(engine->ctx, global);

    if (JS_IsException(result)) {
        throw_current_js_exception(env, engine->ctx);
        return NULL;
    }

    JSContext *job_exception_ctx = NULL;
    run_pending_jobs(engine, &job_exception_ctx);

    Utf8String result_utf8;
    if (!js_value_to_utf8(engine->ctx, result, &result_utf8)) {
        JS_FreeValue(engine->ctx, result);
        if (JS_HasException(engine->ctx)) {
            throw_current_js_exception(env, engine->ctx);
        }
        return NULL;
    }
    JS_FreeValue(engine->ctx, result);

    jstring jresult = utf8_to_jstring(env, result_utf8.bytes, result_utf8.len);
    free(result_utf8.bytes);
    return jresult;
}

/* ── native: setCallback ─────────────────────────────────────────── */

static void native_set_callback(JNIEnv *env, jclass cls, jlong handle,
                                 jstring name, jobject callback)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return;

    if (!callback) {
        throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "callback must not be null");
        return;
    }

    Utf8String name_utf8;
    if (!jstring_to_utf8(env, name, &name_utf8)) return;

    jclass js_func_class = (*env)->FindClass(env, JSFUNCTION_CLASS);
    if (!js_func_class) {
        free(name_utf8.bytes);
        return;
    }
    jmethodID method = (*env)->GetMethodID(env, js_func_class, "invoke",
                                            "([Ljava/lang/String;)Ljava/lang/String;");
    if (!method) {
        free(name_utf8.bytes);
        return;
    }

    jobject global_ref = (*env)->NewGlobalRef(env, callback);

    int slot = register_callback(engine->vm, global_ref, method);
    if (slot < 0) {
        (*env)->DeleteGlobalRef(env, global_ref);
        free(name_utf8.bytes);
        throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "too many callbacks");
        return;
    }

    JS_UpdateStackTop(engine->rt);
    JSValue func = JS_NewCFunctionMagic(engine->ctx, js_java_callback,
                                         name_utf8.bytes, 0,
                                         JS_CFUNC_generic_magic, slot);

    JSValue global = JS_GetGlobalObject(engine->ctx);
    JS_SetPropertyStr(engine->ctx, global, name_utf8.bytes, func);
    JS_FreeValue(engine->ctx, global);
    free(name_utf8.bytes);
}

/* ── native: setConsoleLog / setConsoleError ─────────────────────── */

static void native_set_console_handler(JNIEnv *env, jclass cls, jlong handle,
                                        jobject handler, int type)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return;

    struct {
        JavaVM *vm;
        jobject *handler_ptr;
        jmethodID *method_ptr;
    } targets[] = {
        { engine->vm, &engine->console_log.handler,   &engine->console_log.accept_method },
        { engine->vm, &engine->console_error.handler, &engine->console_error.accept_method },
    };

    int idx = (type >= 0 && type < 2) ? type : 0;

    if (*targets[idx].handler_ptr) {
        (*env)->DeleteGlobalRef(env, *targets[idx].handler_ptr);
        *targets[idx].handler_ptr = NULL;
        *targets[idx].method_ptr = NULL;
    }

    if (!handler) return;

    jclass consumer_class = (*env)->FindClass(env, CONSUMER_CLASS);
    if (!consumer_class) return;
    jmethodID accept_method = (*env)->GetMethodID(env, consumer_class, "accept",
                                                    "(Ljava/lang/Object;)V");
    if (!accept_method) return;

    *targets[idx].handler_ptr = (*env)->NewGlobalRef(env, handler);
    *targets[idx].method_ptr = accept_method;
}

static void native_set_console_log(JNIEnv *env, jclass cls, jlong handle, jobject handler)
{
    native_set_console_handler(env, cls, handle, handler, 0);
}

static void native_set_console_error(JNIEnv *env, jclass cls, jlong handle, jobject handler)
{
    native_set_console_handler(env, cls, handle, handler, 1);
}

/* ── native: compile / load ──────────────────────────────────────── */

static jbyteArray native_compile(JNIEnv *env, jclass cls, jlong handle,
                                  jstring source, jstring filename, jboolean module)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return NULL;

    Utf8String source_utf8;
    Utf8String filename_utf8;
    if (!jstring_to_utf8(env, source, &source_utf8)) return NULL;
    if (!jstring_to_utf8(env, filename, &filename_utf8)) {
        free(source_utf8.bytes);
        return NULL;
    }

    int flags = (module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL) | JS_EVAL_FLAG_COMPILE_ONLY;
    JS_UpdateStackTop(engine->rt);
    JSValue bytecode = JS_Eval(engine->ctx, source_utf8.bytes, source_utf8.len,
                                filename_utf8.bytes, flags);
    free(source_utf8.bytes);
    free(filename_utf8.bytes);

    if (JS_IsException(bytecode)) {
        throw_current_js_exception(env, engine->ctx);
        return NULL;
    }

    size_t buf_len;
    uint8_t *buf = JS_WriteObject(engine->ctx, &buf_len, bytecode, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(engine->ctx, bytecode);

    if (!buf) {
        if (JS_HasException(engine->ctx)) {
            throw_current_js_exception(env, engine->ctx);
        } else {
            throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "failed to serialize bytecode");
        }
        return NULL;
    }

    jbyteArray result = (*env)->NewByteArray(env, (jsize)buf_len);
    if (result) {
        (*env)->SetByteArrayRegion(env, result, 0, (jsize)buf_len, (const jbyte *)buf);
    }
    js_free(engine->ctx, buf);
    return result;
}

static void native_load(JNIEnv *env, jclass cls, jlong handle, jbyteArray bytecode)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return;

    jsize len = (*env)->GetArrayLength(env, bytecode);
    jbyte *buf = (*env)->GetByteArrayElements(env, bytecode, NULL);
    if (!buf) return;

    JS_UpdateStackTop(engine->rt);
    JSValue obj = JS_ReadObject(engine->ctx, (const uint8_t *)buf, (size_t)len,
                                 JS_READ_OBJ_BYTECODE);
    (*env)->ReleaseByteArrayElements(env, bytecode, buf, JNI_ABORT);

    if (JS_IsException(obj)) {
        throw_current_js_exception(env, engine->ctx);
        return;
    }

    JSValue result = JS_EvalFunction(engine->ctx, obj);
    if (JS_IsException(result)) {
        throw_current_js_exception(env, engine->ctx);
        return;
    }

    JSContext *job_exception_ctx = NULL;
    run_pending_jobs(engine, &job_exception_ctx);
    JS_FreeValue(engine->ctx, result);
}

/* ── method table ────────────────────────────────────────────────── */

static JNINativeMethod methods[] = {
    { "nativeCreate",      "(JJ)J",                                              (void *)native_create },
    { "nativeClose",       "(J)V",                                               (void *)native_close },
    { "nativeEval",        "(JLjava/lang/String;Ljava/lang/String;ZJ)Ljava/lang/String;", (void *)native_eval },
    { "nativeEvalAsJson",  "(JLjava/lang/String;Ljava/lang/String;ZJ)Ljava/lang/String;", (void *)native_eval_as_json },
    { "nativeGc",          "(J)V",                                               (void *)native_gc },
    { "nativeVersion",     "()Ljava/lang/String;",                               (void *)native_version },
    { "nativeGetGlobal",   "(JLjava/lang/String;)Ljava/lang/String;",            (void *)native_get_global },
    { "nativeSetGlobal",   "(JLjava/lang/String;Ljava/lang/String;)V",           (void *)native_set_global },
    { "nativeCall",        "(JLjava/lang/String;[Ljava/lang/String;)Ljava/lang/String;", (void *)native_call },
    { "nativeSetCallback", "(JLjava/lang/String;Lio/github/quickjsng/JsFunction;)V", (void *)native_set_callback },
    { "nativeSetConsoleLog",   "(JLjava/util/function/Consumer;)V",              (void *)native_set_console_log },
    { "nativeSetConsoleError", "(JLjava/util/function/Consumer;)V",              (void *)native_set_console_error },
    { "nativeCompile",     "(JLjava/lang/String;Ljava/lang/String;Z)[B",        (void *)native_compile },
    { "nativeLoad",        "(J[B)V",                                             (void *)native_load },
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;

    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    jclass cls = (*env)->FindClass(env, QUICKJS_CLASS);
    if (!cls) {
        return JNI_ERR;
    }

    if ((*env)->RegisterNatives(env, cls, methods, (jint)countof(methods)) != 0) {
        return JNI_ERR;
    }

    return JNI_VERSION_1_6;
}
