package io.github.quickjsng;

import java.io.*;
import java.nio.file.*;

final class NativeLoader {

    private static final String LIB_NAME = "quickjs_jni";
    private static boolean loaded = false;

    static synchronized void load() {
        if (loaded) return;

        String os = normalizeOs(System.getProperty("os.name", ""));
        String arch = normalizeArch(System.getProperty("os.arch", ""));
        String dir = os + "-" + arch;

        String libFile;
        String resourcePath;
        if ("windows".equals(os)) {
            libFile = LIB_NAME + ".dll";
            resourcePath = "/native/" + dir + "/" + libFile;
        } else if ("macos".equals(os)) {
            libFile = "lib" + LIB_NAME + ".dylib";
            resourcePath = "/native/" + dir + "/" + libFile;
        } else {
            libFile = "lib" + LIB_NAME + ".so";
            resourcePath = "/native/" + dir + "/" + libFile;
        }

        try {
            InputStream in = NativeLoader.class.getResourceAsStream(resourcePath);
            if (in == null) {
                System.loadLibrary(LIB_NAME);
                loaded = true;
                return;
            }

            Path tempDir = Files.createTempDirectory("qjs2j-");
            Path tempLib = tempDir.resolve(libFile);
            tempLib.toFile().deleteOnExit();
            tempDir.toFile().deleteOnExit();

            try (OutputStream out = Files.newOutputStream(tempLib)) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) != -1) {
                    out.write(buf, 0, n);
                }
            }
            in.close();

            System.load(tempLib.toAbsolutePath().toString());
            loaded = true;
        } catch (IOException e) {
            throw new UnsatisfiedLinkError("Failed to load native library: " + e.getMessage());
        }
    }

    private static String normalizeOs(String os) {
        os = os.toLowerCase();
        if (os.contains("mac") || os.contains("darwin")) return "macos";
        if (os.contains("linux")) return "linux";
        if (os.contains("win")) return "windows";
        return os;
    }

    private static String normalizeArch(String arch) {
        arch = arch.toLowerCase();
        if (arch.equals("amd64") || arch.equals("x86_64")) return "x86_64";
        if (arch.equals("aarch64") || arch.equals("arm64")) return "arm64";
        if (arch.equals("x86") || arch.equals("i386") || arch.equals("i686")) return "x86";
        return arch;
    }
}
