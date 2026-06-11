# QuickJS Java/Android JNI 绑定

基于 [QuickJS-NG](https://github.com/nicholasgasior/quickjs) 的 Java/Android 嵌入式绑定，通过 JNI 桥接 Java 与 JavaScript 引擎。

## 目录结构

```
qjs2j/
├── CMakeLists.txt                          # 构建配置
├── README.md
├── src/
│   ├── main/
│   │   ├── c/quickjs_jni.c                 # JNI 原生实现
│   │   └── java/io/github/quickjsng/
│   │       ├── QuickJs.java                # 主 API 类
│   │       ├── QuickJsException.java       # JS 异常封装
│   │       └── JsFunction.java             # 回调接口
│   └── test/java/io/github/quickjsng/
│       └── QuickJsTest.java                # 测试用例
└── examples/
    └── HelloQuickJs.java                   # 示例
```

## 快速开始

### 桌面 Java

```bash
# 构建原生库
cmake -S qjs2j -B qjs2j/build -DCMAKE_BUILD_TYPE=Release
cmake --build qjs2j/build

# 编译 Java
javac -d qjs2j/build/classes \
  qjs2j/src/main/java/io/github/quickjsng/*.java \
  qjs2j/examples/HelloQuickJs.java

# 运行
java -Djava.library.path=qjs2j/build \
  -cp qjs2j/build/classes HelloQuickJs
```

### Android

将 `src/main/java/io/github/quickjsng/` 复制到你的 Android 模块中，然后在 `build.gradle` 中配置：

```gradle
android {
    defaultConfig {
        externalNativeBuild {
            cmake {
                arguments "-DQUICKJS_ROOT=/path/to/quickjs"
            }
        }
    }
    externalNativeBuild {
        cmake {
            path "/path/to/qjs2j/CMakeLists.txt"
        }
    }
}
```

## API 参考

### 创建与销毁

```java
// 创建引擎（默认配置）
QuickJs js = new QuickJs();

// 创建引擎（自定义内存限制和栈大小）
QuickJs js = new QuickJs(8L * 1024 * 1024, 512L * 1024);

// 使用 try-with-resources 自动关闭
try (QuickJs js = new QuickJs()) {
    // ...
}

// 手动关闭
js.close();
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `memoryLimitBytes` | `long` | 运行时内存上限（字节），0 = 无限制 |
| `maxStackSizeBytes` | `long` | 最大栈深度（字节），0 = 无限制 |

### 执行 JavaScript

```java
// 基本执行
String result = js.eval("1 + 2");  // "3"

// 指定文件名（影响错误信息中的位置）
String result = js.eval("x + 1", "main.js");

// 执行 ES Module
String result = js.eval("export default 42;", "mod.mjs", true);

// 带超时（毫秒）
String result = js.eval("while(true){}", "<eval>", false, 1000);
// 超时抛出 QuickJsException
```

### 执行并返回 JSON

```java
// 返回值自动 JSON 序列化
String json = js.evalAsJson("({name: 'test', value: 42})");
// json = '{"name":"test","value":42}'

// 对象/数组/基本类型均可
js.evalAsJson("[1, 2, 3]");       // "[1,2,3]"
js.evalAsJson("'hello'");          // "\"hello\""
js.evalAsJson("42");               // "42"
js.evalAsJson("true");             // "true"
js.evalAsJson("undefined");        // null
```

### 全局变量读写

值以 JSON 字符串形式传递，支持所有 JSON 类型：

```java
// 设置
js.setGlobal("name", "\"world\"");      // 字符串
js.setGlobal("count", "42");            // 数字
js.setGlobal("flag", "true");           // 布尔
js.setGlobal("data", "{\"x\":1}");     // 对象
js.setGlobal("list", "[1, 2, 3]");     // 数组
js.setGlobal("nothing", "null");        // null

// 读取（返回 JSON 字符串，不存在返回 null）
String name = js.getGlobal("name");     // "\"world\""
String count = js.getGlobal("count");   // "42"
String missing = js.getGlobal("nope");  // null

// 在 JS 中使用
js.eval("console.log(name)");  // world
```

### 调用 JS 函数

```java
// 定义函数
js.eval("function add(a, b) { return (Number(a) + Number(b)).toString(); }");

// 从 Java 调用
String sum = js.call("add", "3", "4");  // "7"

// 无参数
js.eval("function greet() { return 'hi'; }");
String hi = js.call("greet");  // "hi"

// 可变参数
js.eval("function join() { return Array.from(arguments).join('-'); }");
String joined = js.call("join", "a", "b", "c");  // "a-b-c"
```

### Java 回调（从 JS 调用 Java）

实现 `JsFunction` 接口，所有参数以字符串数组传入，返回字符串：

```java
import io.github.quickjsng.JsFunction;

// 基本回调
js.setCallback("greet", args -> "Hello, " + args[0] + "!");
js.eval("greet('World')");  // "Hello, World!"

// 多参数
js.setCallback("add", args ->
    String.valueOf(Integer.parseInt(args[0]) + Integer.parseInt(args[1])));
js.eval("add(3, 4)");  // "7"

// 返回 null 等同于 JS 的 undefined
js.setCallback("noop", args -> null);
js.eval("typeof noop()");  // "undefined"

// 回调中抛出异常会传播到 JS
js.setCallback("fail", args -> {
    throw new RuntimeException("error from Java");
});
js.eval("fail()");  // 抛出 QuickJsException
```

> **线程安全**：回调可能在任意线程被调用。如果回调来自非 JVM 线程，会自动 `AttachCurrentThread`。

### Console 重定向

将 `console.log` / `console.error` 输出重定向到 Java（Android 适配关键）：

```java
import java.util.function.Consumer;

// 重定向 console.log
js.setConsoleLog(msg -> android.util.Log.d("JS", msg));

// 重定向 console.error
js.setConsoleError(msg -> android.util.Log.e("JS", msg));

// 清除处理器（恢复默认 stdout/stderr）
js.setConsoleLog(null);
js.setConsoleError(null);
```

> **注意**：`console.log`、`console.info`、`console.debug` 走 log 处理器；`console.warn`、`console.error` 走 error 处理器。

### 字节码编译与加载

预编译 JavaScript 为字节码，加速后续执行：

```java
// 编译
byte[] bytecode = js.compile("'hello' + ' world'");
byte[] bytecode = js.compile("import mod from './mod.mjs'", "main.mjs", true);

// 在另一个引擎中加载执行
try (QuickJs js2 = new QuickJs()) {
    js2.load(bytecode);
    String result = js2.eval("'hello' + ' world'");
}
```

### 垃圾回收

```java
// 手动触发 GC
js.gc();
```

### 版本信息

```java
String version = QuickJs.version();  // "0.15.1"
```

## 异常处理

| 异常类型 | 场景 |
|----------|------|
| `QuickJsException` | JS 语法错误、运行时错误、超时、回调异常 |
| `IllegalStateException` | 对已关闭的引擎调用任何方法 |
| `IllegalArgumentException` | 参数不合法（负数超时等） |
| `OutOfMemoryError` | 引擎创建失败 |

```java
try {
    js.eval("throw new Error('boom')");
} catch (QuickJsException e) {
    System.err.println("JS error: " + e.getMessage());
}
```

## 线程安全

- 所有 `QuickJs` 方法内部加锁，可从多线程调用
- **不要**在多个线程同时使用同一个 `QuickJs` 实例（锁会序列化调用，但语义上不支持并发执行）
- 每个 `QuickJs` 实例对应一个独立的 QuickJS 运行时，可安全创建多个实例

## 运行测试

```bash
# 编译测试
javac -d build/classes \
  src/main/java/io/github/quickjsng/*.java \
  src/test/java/io/github/quickjsng/QuickJsTest.java

# 运行测试
java -Djava.library.path=build \
  -cp build/classes io.github.quickjsng.QuickJsTest
```

当前测试覆盖 81 个用例，涵盖：eval、evalAsJson、全局变量读写、函数调用、Java 回调、Console 重定向、字节码编译/加载、超时、异常处理、UTF-8、内存限制、生命周期管理等。
