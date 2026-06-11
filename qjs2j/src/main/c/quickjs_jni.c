#include "qjs_jni.h"

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    QuickJsNative *engine = opaque;
    if (engine && engine->deadline_ms > 0 && now_ms() >= engine->deadline_ms) {
        return 1;
    }
    return 0;
}

/* ── create / close ──────────────────────────────────────────────── */

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

    if (memory_limit > 0) JS_SetMemoryLimit(engine->rt, (size_t)memory_limit);
    if (stack_size > 0)   JS_SetMaxStackSize(engine->rt, (size_t)stack_size);

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
    install_timers(engine->ctx);
    return (jlong)(intptr_t)engine;
}

static void native_close(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return;

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

    free_all_timers(engine);
    JS_FreeContext(engine->ctx);
    JS_FreeRuntime(engine->rt);
    free(engine);
}

/* ── eval ────────────────────────────────────────────────────────── */

static jstring eval_common(JNIEnv *env, jlong handle, jstring source,
                            jstring filename, jboolean module,
                            jlong timeout_ms, bool as_json)
{
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return NULL;

    Utf8String source_utf8, filename_utf8;
    if (!jstring_to_utf8(env, source, &source_utf8)) return NULL;
    if (!jstring_to_utf8(env, filename, &filename_utf8)) {
        free(source_utf8.bytes);
        return NULL;
    }

    int flags = module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    JS_UpdateStackTop(engine->rt);
    engine->deadline_ms = timeout_ms > 0 ? now_ms() + timeout_ms : 0;

    JSValue result = JS_Eval(engine->ctx, source_utf8.bytes, source_utf8.len,
                              filename_utf8.bytes, flags);
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

static jstring native_eval(JNIEnv *env, jclass cls, jlong handle,
                            jstring source, jstring filename,
                            jboolean module, jlong timeout_ms)
{
    (void)cls;
    return eval_common(env, handle, source, filename, module, timeout_ms, false);
}

static jstring native_eval_as_json(JNIEnv *env, jclass cls, jlong handle,
                                    jstring source, jstring filename,
                                    jboolean module, jlong timeout_ms)
{
    (void)cls;
    return eval_common(env, handle, source, filename, module, timeout_ms, true);
}

/* ── gc / version ────────────────────────────────────────────────── */

static void native_gc(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (engine) JS_RunGC(engine->rt);
}

static jstring native_version(JNIEnv *env, jclass cls)
{
    (void)cls;
    const char *version = JS_GetVersion();
    return utf8_to_jstring(env, version, strlen(version));
}

/* ── getGlobal / setGlobal ───────────────────────────────────────── */

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

/* ── call ────────────────────────────────────────────────────────── */

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

    for (int i = 0; i < argc; i++) JS_FreeValue(engine->ctx, jsargs[i]);
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
        if (JS_HasException(engine->ctx)) throw_current_js_exception(env, engine->ctx);
        return NULL;
    }
    JS_FreeValue(engine->ctx, result);

    jstring jresult = utf8_to_jstring(env, result_utf8.bytes, result_utf8.len);
    free(result_utf8.bytes);
    return jresult;
}

/* ── setCallback ─────────────────────────────────────────────────── */

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
    if (!js_func_class) { free(name_utf8.bytes); return; }
    jmethodID method = (*env)->GetMethodID(env, js_func_class, "invoke",
                                            "([Ljava/lang/String;)Ljava/lang/String;");
    if (!method) { free(name_utf8.bytes); return; }

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

/* ── setConsoleLog / setConsoleError ─────────────────────────────── */

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

/* ── compile / load ──────────────────────────────────────────────── */

static jbyteArray native_compile(JNIEnv *env, jclass cls, jlong handle,
                                  jstring source, jstring filename, jboolean module)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return NULL;

    Utf8String source_utf8, filename_utf8;
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

/* ── drainJobs / runEventLoop / awaitPromise / evalAsync ──────────── */

static jint native_drain_jobs(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return -1;
    JS_UpdateStackTop(engine->rt);
    int r = drain_jobs(engine);
    if (r < 0) throw_current_js_exception(env, engine->ctx);
    return r;
}

static jint native_run_event_loop(JNIEnv *env, jclass cls, jlong handle, jlong max_wait_ms)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return -1;
    JS_UpdateStackTop(engine->rt);
    int r = run_event_loop(engine, max_wait_ms);
    if (r < 0) throw_current_js_exception(env, engine->ctx);
    return r;
}

static jstring native_await_promise(JNIEnv *env, jclass cls, jlong handle,
                                     jstring source, jlong timeout_ms)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return NULL;

    Utf8String source_utf8;
    if (!jstring_to_utf8(env, source, &source_utf8)) return NULL;

    JS_UpdateStackTop(engine->rt);
    JSValue result = JS_Eval(engine->ctx, source_utf8.bytes, source_utf8.len,
                              "<await>", JS_EVAL_TYPE_GLOBAL);
    free(source_utf8.bytes);

    if (JS_IsException(result)) {
        throw_current_js_exception(env, engine->ctx);
        return NULL;
    }

    if (!JS_IsPromise(result)) {
        Utf8String out;
        if (!js_value_to_utf8(engine->ctx, result, &out)) {
            JS_FreeValue(engine->ctx, result);
            return NULL;
        }
        JS_FreeValue(engine->ctx, result);
        jstring jstr = utf8_to_jstring(env, out.bytes, out.len);
        free(out.bytes);
        return jstr;
    }

    int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    while (JS_PromiseState(engine->ctx, result) == JS_PROMISE_PENDING) {
        process_timers(engine);
        JSContext *job_ctx = NULL;
        int r = JS_ExecutePendingJob(engine->rt, &job_ctx);
        if (r < 0) {
            JS_FreeValue(engine->ctx, result);
            throw_current_js_exception(env, job_ctx ? job_ctx : engine->ctx);
            return NULL;
        }
        if (r == 0) {
            if (deadline > 0 && now_ms() >= deadline) {
                JS_FreeValue(engine->ctx, result);
                throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "timeout waiting for promise");
                return NULL;
            }
            engine_sleep_ms(1);
        }
    }

    JSPromiseStateEnum state = JS_PromiseState(engine->ctx, result);
    JSValue promise_result = JS_PromiseResult(engine->ctx, result);
    JS_FreeValue(engine->ctx, result);

    if (state == JS_PROMISE_REJECTED) {
        Utf8String err;
        if (js_value_to_utf8(engine->ctx, promise_result, &err)) {
            throw_quickjs_exception(env, err.bytes, err.len);
            free(err.bytes);
        }
        JS_FreeValue(engine->ctx, promise_result);
        return NULL;
    }

    Utf8String out;
    if (!js_value_to_utf8(engine->ctx, promise_result, &out)) {
        JS_FreeValue(engine->ctx, promise_result);
        return NULL;
    }
    JS_FreeValue(engine->ctx, promise_result);
    jstring jstr = utf8_to_jstring(env, out.bytes, out.len);
    free(out.bytes);
    return jstr;
}

static jstring native_eval_async(JNIEnv *env, jclass cls, jlong handle,
                                  jstring source, jstring filename, jlong timeout_ms)
{
    (void)cls;
    QuickJsNative *engine = from_handle(env, handle);
    if (!engine) return NULL;

    Utf8String src, fn;
    if (!jstring_to_utf8(env, source, &src)) return NULL;
    if (!jstring_to_utf8(env, filename, &fn)) { free(src.bytes); return NULL; }

    size_t buf_len = src.len + 64;
    char *buf = malloc(buf_len);
    if (!buf) { free(src.bytes); free(fn.bytes); return NULL; }
    snprintf(buf, buf_len, "(async function(){return %s})()", src.bytes);
    free(src.bytes);

    JS_UpdateStackTop(engine->rt);
    JSValue result = JS_Eval(engine->ctx, buf, strlen(buf), fn.bytes, JS_EVAL_TYPE_GLOBAL);
    free(buf);
    free(fn.bytes);

    if (JS_IsException(result)) {
        throw_current_js_exception(env, engine->ctx);
        return NULL;
    }

    if (JS_IsPromise(result)) {
        int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
        while (JS_PromiseState(engine->ctx, result) == JS_PROMISE_PENDING) {
            process_timers(engine);
            JSContext *job_ctx = NULL;
            int r = JS_ExecutePendingJob(engine->rt, &job_ctx);
            if (r < 0) {
                JS_FreeValue(engine->ctx, result);
                throw_current_js_exception(env, job_ctx ? job_ctx : engine->ctx);
                return NULL;
            }
            if (r == 0) {
                if (deadline > 0 && now_ms() >= deadline) {
                    JS_FreeValue(engine->ctx, result);
                    throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "timeout waiting for async result");
                    return NULL;
                }
                engine_sleep_ms(1);
            }
        }

        JSPromiseStateEnum state = JS_PromiseState(engine->ctx, result);
        JSValue pr = JS_PromiseResult(engine->ctx, result);
        JS_FreeValue(engine->ctx, result);

        if (state == JS_PROMISE_REJECTED) {
            Utf8String err;
            if (js_value_to_utf8(engine->ctx, pr, &err)) {
                throw_quickjs_exception(env, err.bytes, err.len);
                free(err.bytes);
            }
            JS_FreeValue(engine->ctx, pr);
            return NULL;
        }
        result = pr;
    }

    Utf8String out;
    if (!js_value_to_utf8(engine->ctx, result, &out)) {
        JS_FreeValue(engine->ctx, result);
        if (JS_HasException(engine->ctx))
            throw_current_js_exception(env, engine->ctx);
        return NULL;
    }
    JS_FreeValue(engine->ctx, result);
    jstring jstr = utf8_to_jstring(env, out.bytes, out.len);
    free(out.bytes);
    return jstr;
}

/* ── method table + JNI_OnLoad ───────────────────────────────────── */

static JNINativeMethod methods[] = {
    { "nativeCreate",       "(JJ)J",                                              (void *)native_create },
    { "nativeClose",        "(J)V",                                               (void *)native_close },
    { "nativeEval",         "(JLjava/lang/String;Ljava/lang/String;ZJ)Ljava/lang/String;", (void *)native_eval },
    { "nativeEvalAsJson",   "(JLjava/lang/String;Ljava/lang/String;ZJ)Ljava/lang/String;", (void *)native_eval_as_json },
    { "nativeGc",           "(J)V",                                               (void *)native_gc },
    { "nativeVersion",      "()Ljava/lang/String;",                               (void *)native_version },
    { "nativeGetGlobal",    "(JLjava/lang/String;)Ljava/lang/String;",            (void *)native_get_global },
    { "nativeSetGlobal",    "(JLjava/lang/String;Ljava/lang/String;)V",           (void *)native_set_global },
    { "nativeCall",         "(JLjava/lang/String;[Ljava/lang/String;)Ljava/lang/String;", (void *)native_call },
    { "nativeSetCallback",  "(JLjava/lang/String;Lio/github/quickjsng/JsFunction;)V", (void *)native_set_callback },
    { "nativeSetConsoleLog",   "(JLjava/util/function/Consumer;)V",              (void *)native_set_console_log },
    { "nativeSetConsoleError", "(JLjava/util/function/Consumer;)V",              (void *)native_set_console_error },
    { "nativeCompile",      "(JLjava/lang/String;Ljava/lang/String;Z)[B",        (void *)native_compile },
    { "nativeLoad",         "(J[B)V",                                             (void *)native_load },
    { "nativeDrainJobs",    "(J)I",                                              (void *)native_drain_jobs },
    { "nativeRunEventLoop", "(JJ)I",                                             (void *)native_run_event_loop },
    { "nativeAwaitPromise", "(JLjava/lang/String;J)Ljava/lang/String;",          (void *)native_await_promise },
    { "nativeEvalAsync",    "(JLjava/lang/String;Ljava/lang/String;J)Ljava/lang/String;", (void *)native_eval_async },
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
