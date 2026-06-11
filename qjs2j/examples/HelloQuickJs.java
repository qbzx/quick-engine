import io.github.quickjsng.QuickJs;

public final class HelloQuickJs {
    public static void main(String[] args) {
        try (QuickJs js = new QuickJs(8L * 1024 * 1024, 512L * 1024)) {
            System.out.println("QuickJS " + QuickJs.version());

            System.out.println(js.eval("'hello ' + 'quickjs'"));
            System.out.println(js.evalAsJson("({ answer: 6 * 7, items: [1, 2, 3] })"));

            js.setConsoleLog(msg -> System.out.println("[JS] " + msg));
            js.eval("console.log('console.log from JS:', 123)");

            js.setGlobal("name", "\"world\"");
            System.out.println(js.eval("'hello ' + name"));

            js.setGlobal("data", "{\"x\":1,\"y\":2}");
            System.out.println(js.eval("JSON.stringify(data)"));

            js.eval("function add(a, b) { return (Number(a) + Number(b)).toString(); }");
            System.out.println(js.call("add", "3", "4"));

            js.setCallback("javaGreet", args1 -> "Hello from Java, " + args1[0]);
            System.out.println(js.eval("javaGreet('QuickJS')"));

            byte[] bytecode = js.compile("'bytecode ' + 'test'");
            try (QuickJs js2 = new QuickJs()) {
                js2.load(bytecode);
                System.out.println(js2.eval("'bytecode ' + 'test'"));
            }
        }
    }
}
