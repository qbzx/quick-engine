#include "qjs_jni.h"

void emit_console_message(QuickJsNative *engine, int type,
                           const char *msg, size_t msg_len)
{
    struct {
        JavaVM *vm;
        jobject *handler_ptr;
        jmethodID *method_ptr;
    } handlers[] = {
        { engine->console_log.vm,   &engine->console_log.handler,   &engine->console_log.accept_method },
        { engine->console_error.vm, &engine->console_error.handler, &engine->console_error.accept_method },
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
        if (!s) return JS_EXCEPTION;
        total += len;
        if (i > 0) total += 1;
        JS_FreeCString(ctx, s);
    }

    char *buf = malloc(total + 1);
    if (!buf) return JS_EXCEPTION;

    char *p = buf;
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s) { free(buf); return JS_EXCEPTION; }
        if (i > 0) *p++ = ' ';
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
        if (!s) return JS_EXCEPTION;
        total += len;
        if (i > 0) total += 1;
        JS_FreeCString(ctx, s);
    }

    char *buf = malloc(total + 1);
    if (!buf) return JS_EXCEPTION;

    char *p = buf;
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s) { free(buf); return JS_EXCEPTION; }
        if (i > 0) *p++ = ' ';
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

void install_console(JSContext *ctx)
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
