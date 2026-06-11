#include "qjs_jni.h"

int64_t now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

QuickJsNative *from_handle(JNIEnv *env, jlong handle)
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

void throw_exception_string(JNIEnv *env, const char *class_name, const char *message)
{
    jclass cls = (*env)->FindClass(env, class_name);
    if (cls) {
        (*env)->ThrowNew(env, cls, message);
    }
}

/* ── UTF-8 codec ─────────────────────────────────────────────────── */

static uint32_t replacement_codepoint(void)
{
    return 0xfffd;
}

static size_t utf8_len(uint32_t cp)
{
    if (cp <= 0x7f)   return 1;
    if (cp <= 0x7ff)  return 2;
    if (cp <= 0xffff) return 3;
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

bool jstring_to_utf8(JNIEnv *env, jstring input, Utf8String *out)
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

jstring utf8_to_jstring(JNIEnv *env, const char *input, size_t len)
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

/* ── exception helpers ───────────────────────────────────────────── */

void throw_quickjs_exception(JNIEnv *env, const char *message, size_t message_len)
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

bool js_value_to_utf8(JSContext *ctx, JSValueConst value, Utf8String *out)
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

void throw_current_js_exception(JNIEnv *env, JSContext *ctx)
{
    Utf8String message = js_exception_to_utf8(ctx);
    if (message.bytes) {
        throw_quickjs_exception(env, message.bytes, message.len);
        free(message.bytes);
    } else {
        throw_exception_string(env, QUICKJS_EXCEPTION_CLASS, "JavaScript exception");
    }
}
