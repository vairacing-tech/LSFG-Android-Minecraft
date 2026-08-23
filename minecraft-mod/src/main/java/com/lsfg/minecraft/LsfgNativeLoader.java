package com.lsfg.minecraft;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public final class LsfgNativeLoader {

    private static final String LIB_NAME = "liblsfg-minecraft.so";
    private static final String RESOURCE_PATH = "/native/android-arm64/" + LIB_NAME;

    private static String extractedPath = null;

    public static String getExtractedPath() {
        return extractedPath;
    }

    public static boolean load() {
        if (LsfgNativeBridge.isLoaded()) {
            return true;
        }

        if (!LsfgPlatform.isSupportedPlatform()) {
            System.out.println("[LSFG] Native Frame Generation backend unsupported on this platform (" +
                LsfgPlatform.getOs() + "/" + LsfgPlatform.getArch() + "). LSFG disabled.");
            return false;
        }

        File targetDir = resolveExecutableDirectory();
        if (targetDir == null || (!targetDir.exists() && !targetDir.mkdirs())) {
            System.err.println("[LSFG] Error: Could not resolve a writable executable directory for native library extraction.");
            return false;
        }

        File targetFile = new File(targetDir, LIB_NAME);
        extractedPath = targetFile.getAbsolutePath();
        System.out.println("[LSFG] Native backend extraction path: " + extractedPath);

        try (InputStream in = LsfgNativeLoader.class.getResourceAsStream(RESOURCE_PATH)) {
            if (in == null) {
                System.err.println("[LSFG] Error: Native binary " + RESOURCE_PATH + " not found inside JAR resources.");
                return false;
            }

            try (OutputStream out = new FileOutputStream(targetFile)) {
                byte[] buffer = new byte[16384];
                int read;
                while ((read = in.read(buffer)) != -1) {
                    out.write(buffer, 0, read);
                }
                out.flush();
            }

            targetFile.setReadable(true, false);
            targetFile.setExecutable(true, false);

            System.load(targetFile.getAbsolutePath());
            LsfgNativeBridge.setLoaded(true);

            String version = LsfgNativeBridge.getNativeVersion();
            System.out.println("[LSFG] Native backend loaded: " + version);
            return true;
        } catch (Throwable t) {
            System.err.println("[LSFG] Failed to extract or load " + LIB_NAME + ": " + t.getMessage());
            return false;
        }
    }

    private static File resolveExecutableDirectory() {
        // Priority 1: java.io.tmpdir (set to app private cache by Amethyst/Pojav)
        String tmpProp = System.getProperty("java.io.tmpdir");
        if (tmpProp != null && !tmpProp.isEmpty()) {
            File f = new File(tmpProp, "lsfg-native");
            if (f.exists() || f.mkdirs()) return f;
        }

        // Priority 2: TMPDIR environment variable
        String tmpEnv = System.getenv("TMPDIR");
        if (tmpEnv != null && !tmpEnv.isEmpty()) {
            File f = new File(tmpEnv, "lsfg-native");
            if (f.exists() || f.mkdirs()) return f;
        }

        // Priority 3: POJAV_NATIVEDIR
        String pojavNative = System.getenv("POJAV_NATIVEDIR");
        if (pojavNative != null && !pojavNative.isEmpty()) {
            File f = new File(pojavNative);
            if (f.exists() && f.canWrite()) return f;
        }

        return null;
    }
}
