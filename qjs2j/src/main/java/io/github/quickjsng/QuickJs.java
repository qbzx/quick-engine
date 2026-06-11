package io.github.quickjsng;

import java.util.Objects;
import java.util.function.Consumer;

public final class QuickJs implements AutoCloseable {
    static {
        System.loadLibrary("quickjs_jni");
    }

    private final Object lock = new Object();
    private long handle;

    public QuickJs() {
        this(0, 0);
    }

    public QuickJs(long memoryLimitBytes, long maxStackSizeBytes) {
        if (memoryLimitBytes < 0) {
            throw new IllegalArgumentException("memoryLimitBytes must be >= 0");
        }
        if (maxStackSizeBytes < 0) {
            throw new IllegalArgumentException("maxStackSizeBytes must be >= 0");
        }
        handle = nativeCreate(memoryLimitBytes, maxStackSizeBytes);
        if (handle == 0) {
            throw new OutOfMemoryError("Unable to create QuickJS runtime");
        }
    }

    public static String version() {
        return nativeVersion();
    }

    public String eval(String source) {
        return eval(source, "<eval>", false, 0);
    }

    public String eval(String source, String filename) {
        return eval(source, filename, false, 0);
    }

    public String eval(String source, String filename, boolean module) {
        return eval(source, filename, module, 0);
    }

    public String eval(String source, String filename, boolean module, long timeoutMillis) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(filename, "filename");
        if (timeoutMillis < 0) {
            throw new IllegalArgumentException("timeoutMillis must be >= 0");
        }
        synchronized (lock) {
            return nativeEval(requireHandle(), source, filename, module, timeoutMillis);
        }
    }

    public String evalAsJson(String source) {
        return evalAsJson(source, "<eval>", false, 0);
    }

    public String evalAsJson(String source, String filename) {
        return evalAsJson(source, filename, false, 0);
    }

    public String evalAsJson(String source, String filename, boolean module) {
        return evalAsJson(source, filename, module, 0);
    }

    public String evalAsJson(String source, String filename, boolean module, long timeoutMillis) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(filename, "filename");
        if (timeoutMillis < 0) {
            throw new IllegalArgumentException("timeoutMillis must be >= 0");
        }
        synchronized (lock) {
            return nativeEvalAsJson(requireHandle(), source, filename, module, timeoutMillis);
        }
    }

    public String getGlobal(String name) {
        Objects.requireNonNull(name, "name");
        synchronized (lock) {
            return nativeGetGlobal(requireHandle(), name);
        }
    }

    public void setGlobal(String name, String jsonString) {
        Objects.requireNonNull(name, "name");
        Objects.requireNonNull(jsonString, "jsonString");
        synchronized (lock) {
            nativeSetGlobal(requireHandle(), name, jsonString);
        }
    }

    public String call(String functionName, String... args) {
        Objects.requireNonNull(functionName, "functionName");
        synchronized (lock) {
            return nativeCall(requireHandle(), functionName, args);
        }
    }

    public void setCallback(String name, JsFunction callback) {
        Objects.requireNonNull(name, "name");
        Objects.requireNonNull(callback, "callback");
        synchronized (lock) {
            nativeSetCallback(requireHandle(), name, callback);
        }
    }

    public void setConsoleLog(Consumer<String> handler) {
        synchronized (lock) {
            nativeSetConsoleLog(requireHandle(), handler);
        }
    }

    public void setConsoleError(Consumer<String> handler) {
        synchronized (lock) {
            nativeSetConsoleError(requireHandle(), handler);
        }
    }

    public byte[] compile(String source) {
        return compile(source, "<eval>", false);
    }

    public byte[] compile(String source, String filename, boolean module) {
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(filename, "filename");
        synchronized (lock) {
            return nativeCompile(requireHandle(), source, filename, module);
        }
    }

    public void load(byte[] bytecode) {
        Objects.requireNonNull(bytecode, "bytecode");
        synchronized (lock) {
            nativeLoad(requireHandle(), bytecode);
        }
    }

    public void gc() {
        synchronized (lock) {
            nativeGc(requireHandle());
        }
    }

    @Override
    public void close() {
        synchronized (lock) {
            long current = handle;
            handle = 0;
            if (current != 0) {
                nativeClose(current);
            }
        }
    }

    private long requireHandle() {
        if (handle == 0) {
            throw new IllegalStateException("QuickJs is closed");
        }
        return handle;
    }

    private static native long nativeCreate(long memoryLimitBytes, long maxStackSizeBytes);

    private static native void nativeClose(long handle);

    private static native String nativeEval(
            long handle, String source, String filename, boolean module, long timeoutMillis);

    private static native String nativeEvalAsJson(
            long handle, String source, String filename, boolean module, long timeoutMillis);

    private static native void nativeGc(long handle);

    private static native String nativeVersion();

    private static native String nativeGetGlobal(long handle, String name);

    private static native void nativeSetGlobal(long handle, String name, String jsonString);

    private static native String nativeCall(long handle, String functionName, String[] args);

    private static native void nativeSetCallback(long handle, String name, JsFunction callback);

    private static native void nativeSetConsoleLog(long handle, Consumer<String> handler);

    private static native void nativeSetConsoleError(long handle, Consumer<String> handler);

    private static native byte[] nativeCompile(
            long handle, String source, String filename, boolean module);

    private static native void nativeLoad(long handle, byte[] bytecode);
}
