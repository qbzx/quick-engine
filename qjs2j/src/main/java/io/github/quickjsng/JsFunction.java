package io.github.quickjsng;

@FunctionalInterface
public interface JsFunction {
    String invoke(String[] args);
}
