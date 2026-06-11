# qjs2j — QuickJS Java/Android JNI 绑定

基于 [QuickJS-NG](https://github.com/nicholasgasior/quickjs) 的 Java/Android 嵌入式绑定，通过 JNI 桥接 Java 与 JavaScript 引擎。

## 目录

- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [API 参考](#api-参考)
- [进阶用法](#进阶用法)
  - [将 Java 方法映射到 JS](#将-java-方法映射到-js)
  - [构建 JS 工具库](#构建-js-工具库)
  - [插件系统](#插件系统)
  - [脚本沙箱与权限控制](#脚本沙箱与权限控制)
  - [Android 集成示例](#android-集成示例)
  - [事件驱动架构](#事件驱动架构)
  - [字节码预编译加速](#字节码预编译加速)
  - [数据绑定：Java 对象 ↔ JS](#数据绑定java-对象--js)
- [异步编程](#异步编程)
- [异常处理](#异常处理)
- [线程安全](#线程安全)
- [运行测试](#运行测试)

## 目录结构

```
qjs2j/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main/
│   │   ├── c/
│   │   │   ├── qjs_jni.h            # 公共头：类型、宏、函数声明
│   │   │   ├── qjs_util.c           # UTF-8 编解码、异常辅助
│   │   │   ├── qjs_console.c        # console.log/error 重定向
│   │   │   ├── qjs_timer.c          # setTimeout/setInterval + 事件循环
│   │   │   ├── qjs_callback.c       # Java↔JS 回调桥接
│   │   │   └── quickjs_jni.c        # JNI 入口、原生方法
│   │   └── java/io/github/quickjsng/
│   │       ├── QuickJs.java          # 主 API
│   │       ├── QuickJsException.java # JS 异常封装
│   │       └── JsFunction.java       # 回调接口
│   └── test/
│       └── java/io/github/quickjsng/
│           └── QuickJsTest.java      # 91 个测试用例
└── examples/
    └── HelloQuickJs.java
```

## 快速开始

### 桌面 Java

```bash
cmake -S qjs2j -B qjs2j/build -DCMAKE_BUILD_TYPE=Release
cmake --build qjs2j/build

javac -d qjs2j/build/classes \
  qjs2j/src/main/java/io/github/quickjsng/*.java \
  qjs2j/examples/HelloQuickJs.java

java -Djava.library.path=qjs2j/build \
  -cp qjs2j/build/classes HelloQuickJs
```

### Android

将 `src/main/java/io/github/quickjsng/` 复制到 Android 模块中，在 `build.gradle` 中配置：

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
QuickJs js = new QuickJs();                               // 默认配置
QuickJs js = new QuickJs(8L * 1024 * 1024, 512L * 1024); // 自定义内存/栈

try (QuickJs js = new QuickJs()) {
    // 自动关闭
}
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `memoryLimitBytes` | `long` | 运行时内存上限（字节），0 = 无限制 |
| `maxStackSizeBytes` | `long` | 最大栈深度（字节），0 = 无限制 |

### 执行 JavaScript

```java
js.eval("1 + 2");                                        // "3"
js.eval("x + 1", "main.js");                            // 指定文件名
js.eval("export default 42;", "mod.mjs", true);         // ES Module
js.eval("while(true){}", "<eval>", false, 1000);        // 带超时(ms)
```

### 执行并返回 JSON

```java
js.evalAsJson("({name: 'test', value: 42})");  // '{"name":"test","value":42}'
js.evalAsJson("[1, 2, 3]");                     // "[1,2,3]"
js.evalAsJson("undefined");                     // null
```

### 全局变量读写

```java
js.setGlobal("name", "\"world\"");      // 字符串
js.setGlobal("count", "42");            // 数字
js.setGlobal("flag", "true");           // 布尔
js.setGlobal("data", "{\"x\":1}");     // 对象
js.setGlobal("list", "[1, 2, 3]");     // 数组
js.setGlobal("nothing", "null");        // null

String name = js.getGlobal("name");     // "\"world\""
String missing = js.getGlobal("nope");  // null
```

### 调用 JS 函数

```java
js.eval("function add(a, b) { return (Number(a) + Number(b)).toString(); }");
String sum = js.call("add", "3", "4");  // "7"
```

### Java 回调（从 JS 调用 Java）

```java
js.setCallback("greet", args -> "Hello, " + args[0] + "!");
js.eval("greet('World')");  // "Hello, World!"
```

### Console 重定向

```java
js.setConsoleLog(msg -> android.util.Log.d("JS", msg));
js.setConsoleError(msg -> android.util.Log.e("JS", msg));
js.setConsoleLog(null);  // 恢复默认
```

### 字节码编译与加载

```java
byte[] bytecode = js.compile("'hello' + ' world'");
try (QuickJs js2 = new QuickJs()) {
    js2.load(bytecode);
}
```

### 异步

```java
js.drainJobs();                                           // 一次性处理微任务
js.runEventLoop(5000);                                    // 事件循环（带超时）
String r = js.awaitPromise("Promise.resolve(42)", 1000);  // 阻塞等待 Promise
String r = js.evalAsync("await fetch('...')");            // top-level await
```

---

## 进阶用法

### 将 Java 方法映射到 JS

核心模式：通过 `setCallback` 将 Java 方法注册为 JS 全局函数。

#### 1. 简单函数映射

```java
// Java 端：注册一个字符串处理函数
js.setCallback("toUpperCase", args -> args[0].toUpperCase());
js.setCallback("trim", args -> args[0].trim());
js.setCallback("contains", args -> String.valueOf(args[0].contains(args[1])));

// JS 端：直接调用
js.eval("toUpperCase('hello')");       // "HELLO"
js.eval("contains('foobar', 'oob')");  // "true"
```

#### 2. 将 Java 对象的方法映射为 JS 函数

```java
public class StringUtils {
    public static String reverse(String s) {
        return new StringBuilder(s).reverse().toString();
    }
    public static int count(String s, String sub) {
        int count = 0, idx = 0;
        while ((idx = s.indexOf(sub, idx)) != -1) { count++; idx += sub.length(); }
        return count;
    }
}

// 映射
js.setCallback("strReverse", args -> StringUtils.reverse(args[0]));
js.setCallback("strCount", args -> String.valueOf(StringUtils.count(args[0], args[1])));

// JS 使用
js.eval("strReverse('hello')");       // "olleh"
js.eval("strCount('aabaa', 'a')");   // "4"
```

#### 3. 批量注册：用反射自动映射

```java
public class JsBridge {
    /** 将一个对象的所有 public 方法批量注册到 JS */
    public static void registerAll(QuickJs js, String prefix, Object obj) {
        for (var method : obj.getClass().getDeclaredMethods()) {
            method.setAccessible(true);
            String name = prefix + method.getName();
            js.setCallback(name, args -> {
                Object[] javaArgs = new Object[args.length];
                Class<?>[] types = method.getParameterTypes();
                for (int i = 0; i < args.length; i++) {
                    javaArgs[i] = convertArg(args[i], types[i]);
                }
                Object result = method.invoke(obj, javaArgs);
                return result == null ? null : result.toString();
            });
        }
    }

    private static Object convertArg(String jsArg, Class<?> type) {
        if (type == int.class)    return Integer.parseInt(jsArg);
        if (type == long.class)   return Long.parseLong(jsArg);
        if (type == double.class) return Double.parseDouble(jsArg);
        if (type == boolean.class)return Boolean.parseBoolean(jsArg);
        return jsArg; // 默认 String
    }
}

// 使用
public class MathUtils {
    public int add(int a, int b) { return a + b; }
    public double sqrt(double x) { return Math.sqrt(x); }
}

JsBridge.registerAll(js, "math_", new MathUtils());
js.eval("math_add(3, 4)");     // "7"
js.eval("math_sqrt(16)");      // "4.0"
```

#### 4. 将 Java 回调映射为 JS 对象方法

```java
public class ApiClient {
    private String baseUrl;

    public ApiClient(String baseUrl) { this.baseUrl = baseUrl; }

    public String get(String path) {
        // 模拟 HTTP 请求
        return "{\"url\":\"" + baseUrl + path + "\",\"status\":200}";
    }
}

ApiClient client = new ApiClient("https://api.example.com");

// 在 JS 中创建一个对象，其方法调用 Java
js.setCallback("apiGet", args -> client.get(args[0]));
js.eval("""
    var api = {
        get: function(path) { return apiGet(path); },
        getJSON: function(path) { return JSON.parse(apiGet(path)); }
    };
""");

String result = js.eval("api.get('/users')");
// '{"url':'https://api.example.com/users','status':200}'
```

### 构建 JS 工具库

在 Java 端预注入常用工具函数：

```java
public class QuickJsSetup {
    public static QuickJs createEngine() {
        QuickJs js = new QuickJs();

        // 注入工具函数
        js.setCallback("strlen", args -> String.valueOf(args[0].length()));
        js.setCallback("substr", args ->
            args[0].substring(Integer.parseInt(args[1]),
                              Integer.parseInt(args[2])));
        js.setCallback("now", args -> String.valueOf(System.currentTimeMillis()));

        // 注入全局常量
        js.setGlobal("PLATFORM", "\"android\"");
        js.setGlobal("VERSION", "\"1.0.0\"");

        return js;
    }
}

try (QuickJs js = QuickJsSetup.createEngine()) {
    js.eval("console.log('Platform:', PLATFORM)");
    js.eval("console.log('Now:', now())");
}
```

### 插件系统

设计一个简易的 JS 插件机制：

```java
public class PluginLoader {
    private final QuickJs js;

    public PluginLoader(QuickJs js) { this.js = js; }

    /** 加载一个 JS 插件字符串，插件可以调用已注册的 Java API */
    public void loadPlugin(String pluginName, String pluginCode) {
        // 1. 注入插件名称
        js.setGlobal("__plugin_name", "\"" + pluginName + "\"");

        // 2. 执行插件代码（插件可以使用 setCallback 注册的函数）
        js.eval(pluginCode, pluginName + ".js");

        // 3. 调用插件的 init 函数
        try {
            js.call("onPluginInit", pluginName);
        } catch (QuickJsException e) {
            // 插件没有定义 onPluginInit，忽略
        }
    }
}

// 使用
PluginLoader loader = new PluginLoader(js);

// 注入 Java API
js.setCallback("http_get", args -> fetchUrl(args[0]));
js.setCallback("db_query", args -> database.query(args[0]));

// 加载插件
loader.loadPlugin("analytics", """
    function onPluginInit(name) {
        console.log('Plugin loaded:', name);
    }
    function track(event) {
        http_get('https://analytics.example.com/event?' + event);
    }
""");

js.call("track", "page_view");
```

### 脚本沙箱与权限控制

通过回调拦截实现沙箱：

```java
public class Sandbox {
    private final QuickJs js;
    private final Set<String> allowedGlobals;

    public Sandbox(QuickJs js, Set<String> allowedGlobals) {
        this.js = js;
        this.allowedGlobals = allowedGlobals;
    }

    /** 注入受限的 fetch 实现 */
    public void injectFetch() {
        js.setCallback("__fetch", args -> {
            String url = args[0];
            if (!isUrlAllowed(url)) {
                throw new SecurityException("Blocked: " + url);
            }
            return httpGet(url);
        });

        js.eval("""
            var fetch = function(url) {
                return JSON.parse(__fetch(url));
            };
        """);
    }

    /** 注入受限的文件系统访问 */
    public void injectFs() {
        js.setCallback("__readFile", args -> {
            String path = args[0];
            if (path.contains("..")) {
                throw new SecurityException("Path traversal blocked");
            }
            return java.nio.file.Files.readString(
                java.nio.file.Path.of("/sandbox", path));
        });

        js.eval("""
            var fs = {
                readFileSync: function(path) { return __readFile(path); }
            };
        """);
    }

    private boolean isUrlAllowed(String url) {
        return allowedGlobals.stream().anyMatch(url::startsWith);
    }
}

// 使用
Sandbox sandbox = new Sandbox(js, Set.of("https://api.trusted.com/"));
sandbox.injectFetch();
sandbox.injectFs();

js.eval("var data = fetch('https://api.trusted.com/data')");
js.eval("var config = fs.readFileSync('config.json')");
```

### Android 集成示例

```java
public class ScriptEngine {
    private QuickJs js;
    private final Context androidContext;

    public ScriptEngine(Context context) {
        this.androidContext = context;
        this.js = new QuickJs(4L * 1024 * 1024, 256L * 1024);

        // 重定向 console 到 Android Log
        js.setConsoleLog(msg -> Log.d("ScriptEngine", msg));
        js.setConsoleError(msg -> Log.e("ScriptEngine", msg));

        // 注入 Android API
        registerAndroidApis();
    }

    private void registerAndroidApis() {
        // Toast
        js.setCallback("toast", args -> {
            android.os.Handler mainHandler = new android.os.Handler(
                android.os.Looper.getMainLooper());
            mainHandler.post(() ->
                android.widget.Toast.makeText(androidContext,
                    args[0], android.widget.Toast.LENGTH_SHORT).show());
            return null;
        });

        // SharedPreferences 读写
        js.setCallback("pref_get", args -> {
            var prefs = androidContext.getSharedPreferences("app", 0);
            return prefs.getString(args[0], args.length > 1 ? args[1] : null);
        });
        js.setCallback("pref_set", args -> {
            var prefs = androidContext.getSharedPreferences("app", 0);
            prefs.edit().putString(args[0], args[1]).apply();
            return null;
        });

        // 日志
        js.setCallback("log", args -> {
            Log.d("JS", args[0]);
            return null;
        });
    }

    public String runScript(String script) {
        return js.eval(script);
    }

    public void destroy() {
        if (js != null) { js.close(); js = null; }
    }
}

// Activity 中使用
ScriptEngine engine = new ScriptEngine(this);
engine.runScript("toast('Hello from JS!')");
engine.runScript("pref_set('last_run', String(Date.now()))");
String lastRun = engine.runScript("pref_get('last_run', 'never')");
engine.destroy();
```

### 事件驱动架构

利用回调 + 事件循环实现消息驱动的 JS 引擎：

```java
public class EventDrivenEngine {
    private final QuickJs js;
    private final BlockingQueue<String> eventQueue = new LinkedBlockingQueue<>();
    private volatile boolean running = true;

    public EventDrivenEngine() {
        js = new QuickJs();

        // JS 端注册事件处理器
        js.setCallback("__emit", args -> {
            eventQueue.offer(args[0]);
            return null;
        });

        // 启动事件循环线程
        new Thread(this::eventLoop).start();
    }

    public void loadHandler(String event, String handlerCode) {
        js.eval("""
            if (!__handlers) var __handlers = {};
            __handlers['%s'] = %s;
        """.formatted(event, handlerCode));
    }

    private void eventLoop() {
        while (running) {
            String event = eventQueue.poll(100, TimeUnit.MILLISECONDS);
            if (event != null) {
                js.eval("""
                    if (__handlers && __handlers['%s']) {
                        __handlers['%s']();
                    }
                """.formatted(event, event));
            }
        }
    }

    public void shutdown() {
        running = false;
        js.close();
    }
}

// 使用
EventDrivenEngine engine = new EventDrivenEngine();
engine.loadHandler("data_ready", "function() { console.log('Data is ready!'); }");
// 其他线程中触发事件
js.setCallback("emit", args -> { engine.eventQueue.offer(args[0]); return null; });
```

### 字节码预编译加速

首次编译后缓存字节码，后续加载跳过解析：

```java
public class ScriptCache {
    private final Path cacheDir;

    public ScriptCache(Path cacheDir) {
        this.cacheDir = cacheDir;
        Files.createDirectories(cacheDir);
    }

    /** 获取或编译脚本，返回可执行的字节码 */
    public byte[] getOrCompile(QuickJs js, String name, String source) {
        Path cacheFile = cacheDir.resolve(name + ".qjsc");
        if (Files.exists(cacheFile)) {
            return Files.readAllBytes(cacheFile);
        }
        byte[] bytecode = js.compile(source, name + ".js");
        Files.write(cacheFile, bytecode);
        return bytecode;
    }

    /** 在新引擎中执行缓存的字节码 */
    public String execute(byte[] bytecode, long memoryLimit) {
        try (QuickJs js = new QuickJs(memoryLimit, 0)) {
            js.load(bytecode);
            return js.eval("typeof __exports !== 'undefined' ? __exports : undefined");
        }
    }
}
```

### 数据绑定：Java 对象 ↔ JS

通过 JSON 序列化实现双向数据传递：

```java
public class DataBinding {
    private final QuickJs js;

    public DataBinding(QuickJs js) { this.js = js; }

    /** 将 Java Map 注入为 JS 全局变量 */
    public void inject(String name, Map<String, Object> data) {
        String json = new com.google.gson.Gson().toJson(data);
        js.setGlobal(name, json);
    }

    /** 将 Java List 注入为 JS 全局变量 */
    public void inject(String name, List<?> data) {
        String json = new com.google.gson.Gson().toJson(data);
        js.setGlobal(name, json);
    }

    /** 从 JS 获取对象，反序列化为 Map */
    @SuppressWarnings("unchecked")
    public Map<String, Object> getMap(String name) {
        String json = js.getGlobal(name);
        if (json == null) return null;
        return new com.google.gson.Gson().fromJson(json, Map.class);
    }

    /** 从 JS 获取数组，反序列化为 List */
    @SuppressWarnings("unchecked")
    public List<Object> getList(String name) {
        String json = js.getGlobal(name);
        if (json == null) return null;
        return new com.google.gson.Gson().fromJson(json, List.class);
    }
}

// 使用
DataBinding bind = new DataBinding(js);
bind.inject("user", Map.of("name", "Alice", "age", 30));
bind.inject("scores", List.of(95, 87, 92));

js.eval("user.name");   // "Alice"
js.eval("scores[0]");   // "95"

Map<String, Object> user = bind.getMap("user");
// {name=Alice, age=30}
```

---

## 异步编程

### setTimeout / setInterval

```java
js.setConsoleLog(System.out::println);

js.eval("""
    var count = 0;
    var id = setInterval(() => {
        count++;
        console.log('tick', count);
        if (count >= 5) clearInterval(id);
    }, 100);
""");

js.runEventLoop(2000);  // 阻塞运行事件循环，最多 2 秒
```

### Promise 等待

```java
// 同步等待 Promise 结果
String result = js.awaitPromise("""
    new Promise(resolve => {
        setTimeout(() => resolve('done'), 100);
    })
""", 5000);
// result = "done"
```

### Top-level await

```java
String result = js.evalAsync("""
    const response = await fetch('https://api.example.com/data');
    const data = await response.json();
    data.value;
""", 10000);
```

### drainJobs — 非阻塞微任务处理

```java
js.eval("""
    Promise.resolve(42).then(v => {
        globalThis.asyncResult = v;
    });
""");
js.drainJobs();  // 立即执行所有微任务
String r = js.getGlobal("asyncResult");  // "42"
```

---

## 异常处理

| 异常类型 | 场景 |
|----------|------|
| `QuickJsException` | JS 语法错误、运行时错误、超时、回调异常 |
| `IllegalStateException` | 对已关闭的引擎调用任何方法 |
| `IllegalArgumentException` | 参数不合法（负数超时等） |
| `OutOfMemoryError` | 引擎创建失败 |

```java
try {
    js.eval("undefinedFunction()");
} catch (QuickJsException e) {
    System.err.println("JS error: " + e.getMessage());
}
```

## 线程安全

- 所有 `QuickJs` 方法内部加锁，可从多线程调用
- **不要**在多个线程同时使用同一个 `QuickJs` 实例（锁会序列化调用，但语义上不支持并发执行）
- 每个 `QuickJs` 实例对应一个独立的 QuickJS 运行时，可安全创建多个实例
- 回调可能在任意线程被调用，来自非 JVM 线程时会自动 `AttachCurrentThread`

## 运行测试

```bash
# 构建原生库
cmake -S qjs2j -B qjs2j/build -DCMAKE_BUILD_TYPE=Release
cmake --build qjs2j/build

# 编译并运行测试
javac -d qjs2j/build/classes \
  qjs2j/src/main/java/io/github/quickjsng/*.java \
  qjs2j/src/test/java/io/github/quickjsng/QuickJsTest.java

java -Djava.library.path=qjs2j/build \
  -cp qjs2j/build/classes io.github.quickjsng.QuickJsTest
```
