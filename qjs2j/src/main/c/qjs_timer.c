#include "qjs_jni.h"

/* ── timer JS functions ──────────────────────────────────────────── */

static JSValue js_set_timeout(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "setTimeout: first argument must be a function");

    int64_t delay = 0;
    if (argc > 1) JS_ToInt64(ctx, &delay, argv[1]);
    if (delay < 0) delay = 0;

    QuickJsNative *engine = JS_GetContextOpaque(ctx);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!engine->timers[i].active) {
            TimerEntry *t = &engine->timers[i];
            t->id = engine->next_timer_id++;
            t->callback = JS_DupValue(ctx, argv[0]);
            t->delay_ms = delay;
            t->repeat = false;
            t->fire_time_ms = now_ms() + delay;
            t->active = true;
            return JS_NewInt32(ctx, t->id);
        }
    }
    return JS_ThrowInternalError(ctx, "too many timers");
}

static JSValue js_set_interval(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "setInterval: first argument must be a function");

    int64_t delay = 0;
    if (argc > 1) JS_ToInt64(ctx, &delay, argv[1]);
    if (delay < 0) delay = 0;

    QuickJsNative *engine = JS_GetContextOpaque(ctx);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!engine->timers[i].active) {
            TimerEntry *t = &engine->timers[i];
            t->id = engine->next_timer_id++;
            t->callback = JS_DupValue(ctx, argv[0]);
            t->delay_ms = delay;
            t->repeat = true;
            t->fire_time_ms = now_ms() + delay;
            t->active = true;
            return JS_NewInt32(ctx, t->id);
        }
    }
    return JS_ThrowInternalError(ctx, "too many timers");
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_UNDEFINED;

    QuickJsNative *engine = JS_GetContextOpaque(ctx);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (engine->timers[i].active && engine->timers[i].id == id) {
            JS_FreeValue(ctx, engine->timers[i].callback);
            engine->timers[i].callback = JS_UNDEFINED;
            engine->timers[i].active = false;
            break;
        }
    }
    return JS_UNDEFINED;
}

/* ── timer processing ────────────────────────────────────────────── */

void process_timers(QuickJsNative *engine)
{
    int64_t now = now_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        TimerEntry *t = &engine->timers[i];
        if (!t->active || now < t->fire_time_ms) continue;

        JSValue r = JS_Call(engine->ctx, t->callback, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(engine->ctx);
            JS_FreeValue(engine->ctx, exc);
        } else {
            JS_FreeValue(engine->ctx, r);
        }

        if (t->repeat) {
            t->fire_time_ms += t->delay_ms;
            if (t->fire_time_ms < now) t->fire_time_ms = now;
        } else {
            JS_FreeValue(engine->ctx, t->callback);
            t->callback = JS_UNDEFINED;
            t->active = false;
        }
    }
}

static bool has_active_timers(QuickJsNative *engine)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (engine->timers[i].active) return true;
    return false;
}

void free_all_timers(QuickJsNative *engine)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (engine->timers[i].active) {
            JS_FreeValue(engine->ctx, engine->timers[i].callback);
            engine->timers[i].callback = JS_UNDEFINED;
            engine->timers[i].active = false;
        }
    }
}

void install_timers(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, js_clear_timer, "clearInterval", 1));
    JS_FreeValue(ctx, global);
}

/* ── event loop ──────────────────────────────────────────────────── */

void engine_sleep_ms(int64_t ms)
{
    if (ms < 1) ms = 1;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)(ms * 1000));
#endif
}

int drain_jobs(QuickJsNative *engine)
{
    bool progress = true;
    while (progress) {
        progress = false;
        JSContext *ctx = NULL;
        int r;
        while ((r = JS_ExecutePendingJob(engine->rt, &ctx)) > 0)
            progress = true;
        if (r < 0) return -1;

        int64_t now = now_ms();
        for (int i = 0; i < MAX_TIMERS; i++) {
            TimerEntry *t = &engine->timers[i];
            if (!t->active || now < t->fire_time_ms) continue;
            progress = true;
            JSValue rv = JS_Call(engine->ctx, t->callback, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(rv)) {
                JSValue exc = JS_GetException(engine->ctx);
                JS_FreeValue(engine->ctx, exc);
            } else {
                JS_FreeValue(engine->ctx, rv);
            }
            if (t->repeat) {
                t->fire_time_ms += t->delay_ms;
                if (t->fire_time_ms < now) t->fire_time_ms = now;
            } else {
                JS_FreeValue(engine->ctx, t->callback);
                t->callback = JS_UNDEFINED;
                t->active = false;
            }
        }
    }
    return 0;
}

int run_event_loop(QuickJsNative *engine, int64_t max_wait_ms)
{
    int64_t start = now_ms();
    while (1) {
        bool progress = false;
        JSContext *ctx = NULL;
        int r;
        while ((r = JS_ExecutePendingJob(engine->rt, &ctx)) > 0)
            progress = true;
        if (r < 0) return -1;

        int64_t now = now_ms();
        for (int i = 0; i < MAX_TIMERS; i++) {
            TimerEntry *t = &engine->timers[i];
            if (!t->active || now < t->fire_time_ms) continue;
            progress = true;
            JSValue rv = JS_Call(engine->ctx, t->callback, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(rv)) {
                JSValue exc = JS_GetException(engine->ctx);
                JS_FreeValue(engine->ctx, exc);
            } else {
                JS_FreeValue(engine->ctx, rv);
            }
            if (t->repeat) {
                t->fire_time_ms += t->delay_ms;
                if (t->fire_time_ms < now) t->fire_time_ms = now;
            } else {
                JS_FreeValue(engine->ctx, t->callback);
                t->callback = JS_UNDEFINED;
                t->active = false;
            }
        }

        if (!progress) {
            if (!has_active_timers(engine)) return 0;
            if (max_wait_ms > 0 && now_ms() - start >= max_wait_ms) return 0;
            engine_sleep_ms(1);
        } else {
            if (max_wait_ms > 0 && now_ms() - start >= max_wait_ms) return 0;
        }
    }
}

int run_pending_jobs(QuickJsNative *engine, JSContext **exception_ctx)
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
