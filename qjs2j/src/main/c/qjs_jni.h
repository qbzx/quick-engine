#ifndef QJS_JNI_H
#define QJS_JNI_H

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
#include <unistd.h>
#endif

#include "quickjs.h"

#define QUICKJS_CLASS "io/github/quickjsng/QuickJs"
#define QUICKJS_EXCEPTION_CLASS "io/github/quickjsng/QuickJsException"
#define JSFUNCTION_CLASS "io/github/quickjsng/JsFunction"
#define CONSUMER_CLASS "java/util/function/Consumer"
#define countof(x) (sizeof(x) / sizeof((x)[0]))

#define MAX_CALLBACKS 1024
#define MAX_TIMERS 256

/* ── types ───────────────────────────────────────────────────────── */

typedef struct {
    int id;
    JSValue callback;
    int64_t delay_ms;
    bool repeat;
    int64_t fire_time_ms;
    bool active;
} TimerEntry;

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
    TimerEntry timers[MAX_TIMERS];
    int next_timer_id;
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

/* ── util ────────────────────────────────────────────────────────── */

int64_t         now_ms(void);
QuickJsNative  *from_handle(JNIEnv *env, jlong handle);
void            throw_exception_string(JNIEnv *env, const char *class_name, const char *message);
bool            jstring_to_utf8(JNIEnv *env, jstring input, Utf8String *out);
jstring         utf8_to_jstring(JNIEnv *env, const char *input, size_t len);
void            throw_quickjs_exception(JNIEnv *env, const char *message, size_t message_len);
bool            js_value_to_utf8(JSContext *ctx, JSValueConst value, Utf8String *out);
void            throw_current_js_exception(JNIEnv *env, JSContext *ctx);

/* ── console ─────────────────────────────────────────────────────── */

void install_console(JSContext *ctx);
void emit_console_message(QuickJsNative *engine, int type, const char *msg, size_t msg_len);

/* ── timer ───────────────────────────────────────────────────────── */

void install_timers(JSContext *ctx);
void process_timers(QuickJsNative *engine);
void free_all_timers(QuickJsNative *engine);

/* ── event loop ──────────────────────────────────────────────────── */

void engine_sleep_ms(int64_t ms);
int  drain_jobs(QuickJsNative *engine);
int  run_event_loop(QuickJsNative *engine, int64_t max_wait_ms);
int  run_pending_jobs(QuickJsNative *engine, JSContext **exception_ctx);

/* ── callback ────────────────────────────────────────────────────── */

JSValue js_java_callback(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic);
int  register_callback(JavaVM *vm, jobject callback, jmethodID invoke_method);
void unregister_callback(JNIEnv *env, int slot);
extern CallbackSlot g_callbacks[MAX_CALLBACKS];

#endif /* QJS_JNI_H */
