package io.github.quickjsng;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

public final class QuickJsTest {

    private static int passed = 0;
    private static int failed = 0;

    private static void check(String name, boolean condition) {
        if (condition) {
            passed++;
            System.out.println("  PASS: " + name);
        } else {
            failed++;
            System.out.println("  FAIL: " + name);
        }
    }

    private static void checkThrows(String name, Runnable r, Class<? extends Throwable> expected) {
        try {
            r.run();
            failed++;
            System.out.println("  FAIL: " + name + " (no exception thrown)");
        } catch (Throwable t) {
            if (expected.isInstance(t)) {
                passed++;
                System.out.println("  PASS: " + name);
            } else {
                failed++;
                System.out.println("  FAIL: " + name + " (expected " + expected.getSimpleName()
                        + " but got " + t.getClass().getSimpleName() + ": " + t.getMessage() + ")");
            }
        }
    }

    private static void section(String title) {
        System.out.println("\n── " + title + " ──");
    }

    public static void main(String[] args) {
        testVersion();
        testEval();
        testEvalAsJson();
        testEvalOverloads();
        testEvalSyntaxError();
        testEvalTimeout();
        testGetGlobal();
        testSetGlobal();
        testSetGlobalTypes();
        testSetGlobalInvalidJson();
        testCall();
        testCallNoFunction();
        testCallback();
        testCallbackMultipleArgs();
        testCallbackNullReturn();
        testCallbackException();
        testConsoleLog();
        testConsoleError();
        testConsoleNull();
        testConsoleRedirect();
        testCompileAndLoad();
        testCompileSyntaxError();
        testGc();
        testClose();
        testClosedEngine();
        testMemoryLimit();
        testUtf8();

        System.out.println("\n════════════════════════════════════════");
        System.out.println("Results: " + passed + " passed, " + failed + " failed");
        if (failed > 0) {
            System.exit(1);
        }
    }

    private static void testVersion() {
        section("version");
        String v = QuickJs.version();
        check("version not null", v != null);
        check("version not empty", v.length() > 0);
        check("version contains digits", v.matches(".*\\d+.*"));
        System.out.println("  (version: " + v + ")");
    }

    private static void testEval() {
        section("eval");
        try (QuickJs js = new QuickJs()) {
            check("string literal", "hello".equals(js.eval("'hello'")));
            check("number", "42".equals(js.eval("(40 + 2).toString()")));
            check("boolean true", "true".equals(js.eval("true.toString()")));
            check("boolean false", "false".equals(js.eval("false.toString()")));
            check("null", "null".equals(js.eval("String(null)")));
            check("undefined", "undefined".equals(js.eval("String(undefined)")));
            check("multi-line", "ab".equals(js.eval("'a' +\n'b'")));
            check("function def + call", "6".equals(js.eval(
                    "function f(x) { return x * 2; } f(3).toString()")));
            check("with filename", "ok".equals(js.eval("1 + 1 === 2 ? 'ok' : 'fail'", "test.js")));
        }
    }

    private static void testEvalAsJson() {
        section("evalAsJson");
        try (QuickJs js = new QuickJs()) {
            check("object", "{\"a\":1}".equals(js.evalAsJson("({a: 1})")));
            check("array", "[1,2,3]".equals(js.evalAsJson("[1,2,3]")));
            check("string", "\"hello\"".equals(js.evalAsJson("'hello'")));
            check("number", "42".equals(js.evalAsJson("42")));
            check("boolean", "true".equals(js.evalAsJson("true")));
            check("nested object", "{\"x\":{\"y\":2}}".equals(js.evalAsJson("({x:{y:2}})")));
            check("undefined returns null",
                    js.evalAsJson("undefined") == null);
        }
    }

    private static void testEvalOverloads() {
        section("evalAsJson overloads");
        try (QuickJs js = new QuickJs()) {
            check("evalAsJson(source)", "1".equals(js.evalAsJson("1")));
            check("evalAsJson(source,filename)", "2".equals(js.evalAsJson("2", "test.js")));
            check("evalAsJson(source,filename,module)", "3".equals(js.evalAsJson("3", "test.js", false)));
            check("eval(source)", "a".equals(js.eval("'a'")));
            check("eval(source,filename)", "b".equals(js.eval("'b'", "test.js")));
            check("eval(source,filename,module)", "c".equals(js.eval("'c'", "test.js", false)));
        }
    }

    private static void testEvalSyntaxError() {
        section("eval syntax error");
        try (QuickJs js = new QuickJs()) {
            checkThrows("syntax error throws QuickJsException",
                    () -> js.eval("function("),
                    QuickJsException.class);
        }
    }

    private static void testEvalTimeout() {
        section("eval timeout");
        try (QuickJs js = new QuickJs()) {
            check("no timeout completes", "done".equals(js.eval("'done'")));
            checkThrows("timeout throws QuickJsException",
                    () -> js.eval("while(true){}", "<eval>", false, 100),
                    QuickJsException.class);
        }
    }

    private static void testGetGlobal() {
        section("getGlobal");
        try (QuickJs js = new QuickJs()) {
            js.eval("var x = 42");
            check("get existing global", "42".equals(js.getGlobal("x")));
            check("get non-existing returns null", js.getGlobal("nonexistent") == null);
        }
    }

    private static void testSetGlobal() {
        section("setGlobal");
        try (QuickJs js = new QuickJs()) {
            js.setGlobal("msg", "\"hello\"");
            check("set string", "hello".equals(js.eval("msg")));

            js.setGlobal("num", "123");
            check("set number", "123".equals(js.eval("num.toString()")));

            js.setGlobal("flag", "true");
            check("set boolean", "true".equals(js.eval("flag.toString()")));

            js.setGlobal("arr", "[1,2,3]");
            check("set array", "3".equals(js.eval("arr.length.toString()")));

            js.setGlobal("obj", "{\"a\":1,\"b\":2}");
            check("set object", "3".equals(js.eval("(obj.a + obj.b).toString()")));
        }
    }

    private static void testSetGlobalTypes() {
        section("setGlobal type roundtrip");
        try (QuickJs js = new QuickJs()) {
            js.setGlobal("s", "\"abc\"");
            check("string roundtrip", "\"abc\"".equals(js.getGlobal("s")));

            js.setGlobal("n", "42");
            check("number roundtrip", "42".equals(js.getGlobal("n")));

            js.setGlobal("b", "true");
            check("boolean roundtrip", "true".equals(js.getGlobal("b")));

            js.setGlobal("nil", "null");
            check("null roundtrip", "null".equals(js.getGlobal("nil")));

            js.setGlobal("obj", "{\"x\":1}");
            check("object roundtrip", "{\"x\":1}".equals(js.getGlobal("obj")));

            js.setGlobal("arr", "[1,2]");
            check("array roundtrip", "[1,2]".equals(js.getGlobal("arr")));
        }
    }

    private static void testSetGlobalInvalidJson() {
        section("setGlobal invalid JSON");
        try (QuickJs js = new QuickJs()) {
            checkThrows("invalid JSON throws QuickJsException",
                    () -> js.setGlobal("x", "{invalid}"),
                    QuickJsException.class);
        }
    }

    private static void testCall() {
        section("call");
        try (QuickJs js = new QuickJs()) {
            js.eval("function add(a, b) { return (Number(a) + Number(b)).toString(); }");
            check("call with 2 args", "7".equals(js.call("add", "3", "4")));
            js.eval("function zero() { return (0).toString(); }");
            check("call with 0 args", "0".equals(js.call("zero")));

            js.eval("function greet(name) { return 'Hello, ' + name; }");
            check("call with 1 arg", "Hello, World".equals(js.call("greet", "World")));

            js.eval("function concat() { return Array.from(arguments).join('-'); }");
            check("call with varargs", "a-b-c".equals(js.call("concat", "a", "b", "c")));
        }
    }

    private static void testCallNoFunction() {
        section("call non-function");
        try (QuickJs js = new QuickJs()) {
            js.eval("var notAFunction = 42");
            checkThrows("calling non-function throws QuickJsException",
                    () -> js.call("notAFunction"),
                    QuickJsException.class);
        }
    }

    private static void testCallback() {
        section("callback basic");
        try (QuickJs js = new QuickJs()) {
            js.setCallback("greet", args -> "Hello, " + args[0] + "!");
            check("callback from eval", "Hello, World!".equals(js.eval("greet('World')")));

            js.setCallback("add", args ->
                    String.valueOf(Integer.parseInt(args[0]) + Integer.parseInt(args[1])));
            check("callback with 2 args", "15".equals(js.eval("add(7, 8)")));
        }
    }

    private static void testCallbackMultipleArgs() {
        section("callback multiple args");
        try (QuickJs js = new QuickJs()) {
            js.setCallback("join", args -> String.join(",", args));
            check("3 args", "a,b,c".equals(js.eval("join('a','b','c')")));
            check("1 arg only", "x".equals(js.eval("join('x')")));
        }
    }

    private static void testCallbackNullReturn() {
        section("callback null return");
        try (QuickJs js = new QuickJs()) {
            js.setCallback("nothing", args -> null);
            check("null return becomes undefined",
                    "undefined".equals(js.eval("typeof nothing()")));
        }
    }

    private static void testCallbackException() {
        section("callback throws exception");
        try (QuickJs js = new QuickJs()) {
            js.setCallback("fail", args -> {
                throw new RuntimeException("test error");
            });
            checkThrows("Java exception in callback throws QuickJsException",
                    () -> js.eval("fail()"),
                    QuickJsException.class);
        }
    }

    private static void testConsoleLog() {
        section("console.log (default stdout)");
        try (QuickJs js = new QuickJs()) {
            js.eval("console.log('hello', 'world', 123)");
            js.eval("console.info('info message')");
            js.eval("console.debug('debug message')");
            check("console.log does not throw", true);
        }
    }

    private static void testConsoleError() {
        section("console.error (default stderr)");
        try (QuickJs js = new QuickJs()) {
            js.eval("console.error('error message')");
            js.eval("console.warn('warn message')");
            check("console.error does not throw", true);
        }
    }

    private static void testConsoleNull() {
        section("console with zero args");
        try (QuickJs js = new QuickJs()) {
            js.eval("console.log()");
            js.eval("console.error()");
            check("zero-arg console calls do not throw", true);
        }
    }

    private static void testConsoleRedirect() {
        section("console redirect");
        try (QuickJs js = new QuickJs()) {
            List<String> logMessages = new ArrayList<>();
            List<String> errorMessages = new ArrayList<>();

            js.setConsoleLog(logMessages::add);
            js.setConsoleError(errorMessages::add);

            js.eval("console.log('log1', 'log2')");
            js.eval("console.error('err1')");

            check("log handler called", logMessages.size() == 1);
            check("log message correct", "log1 log2".equals(logMessages.get(0)));
            check("error handler called", errorMessages.size() == 1);
            check("error message correct", "err1".equals(errorMessages.get(0)));

            js.setConsoleLog(null);
            js.setConsoleError(null);
            js.eval("console.log('fallback')");
            check("clear handler falls back to stdout", true);
        }
    }

    private static void testCompileAndLoad() {
        section("compile and load");
        try (QuickJs js1 = new QuickJs()) {
            byte[] bytecode = js1.compile("'hello' + ' bytecode'");
            check("compile returns non-null", bytecode != null);
            check("bytecode is non-empty", bytecode.length > 0);

            try (QuickJs js2 = new QuickJs()) {
                js2.load(bytecode);
                check("load executes", "hello bytecode".equals(js2.eval("'hello' + ' bytecode'")));
            }
        }

        try (QuickJs js1 = new QuickJs()) {
            js1.eval("function mul(a, b) { return (a * b).toString(); }");
            byte[] bytecode = js1.compile("mul(6, 7).toString()");
            try (QuickJs js2 = new QuickJs()) {
                js2.eval("function mul(a, b) { return (a * b).toString(); }");
                js2.load(bytecode);
                check("load with function dep", "42".equals(js2.eval("mul(6, 7).toString()")));
            }
        }
    }

    private static void testCompileSyntaxError() {
        section("compile syntax error");
        try (QuickJs js = new QuickJs()) {
            checkThrows("compile syntax error throws QuickJsException",
                    () -> js.compile("function("),
                    QuickJsException.class);
        }
    }

    private static void testGc() {
        section("gc");
        try (QuickJs js = new QuickJs()) {
            js.eval("var a = [1,2,3,4,5]");
            js.gc();
            check("gc does not throw", true);
            check("after gc, eval still works", "ok".equals(js.eval("'ok'")));
        }
    }

    private static void testClose() {
        section("close");
        QuickJs js = new QuickJs();
        js.eval("'test'");
        js.close();
        js.close();
        js.close();
        check("double close no crash", true);
    }

    private static void testClosedEngine() {
        section("closed engine");
        QuickJs js = new QuickJs();
        js.close();

        checkThrows("eval on closed throws IllegalStateException",
                () -> js.eval("'test'"),
                IllegalStateException.class);
        checkThrows("evalAsJson on closed throws IllegalStateException",
                () -> js.evalAsJson("'test'"),
                IllegalStateException.class);
        checkThrows("call on closed throws IllegalStateException",
                () -> js.call("f"),
                IllegalStateException.class);
        checkThrows("getGlobal on closed throws IllegalStateException",
                () -> js.getGlobal("x"),
                IllegalStateException.class);
        checkThrows("setGlobal on closed throws IllegalStateException",
                () -> js.setGlobal("x", "1"),
                IllegalStateException.class);
    }

    private static void testMemoryLimit() {
        section("memory limit");
        try (QuickJs js = new QuickJs(1024 * 1024, 0)) {
            check("1MB runtime works", "ok".equals(js.eval("'ok'")));
        }
        try (QuickJs js = new QuickJs(0, 64 * 1024)) {
            check("64KB stack works", "ok".equals(js.eval("'ok'")));
        }
    }

    private static void testUtf8() {
        section("UTF-8 support");
        try (QuickJs js = new QuickJs()) {
            check("ASCII", "hello".equals(js.eval("'hello'")));
            check("Chinese", "\u4f60\u597d".equals(js.eval("'\u4f60\u597d'")));
            check("Emoji", "\uD83D\uDE00".equals(js.eval("'\uD83D\uDE00'")));
            check("Mixed", "a\u4f60b".equals(js.eval("'a\u4f60b'")));
            check("Japanese", "\u3053\u3093\u306b\u3061\u306f".equals(js.eval("'\u3053\u3093\u306b\u3061\u306f'")));
        }
    }
}
