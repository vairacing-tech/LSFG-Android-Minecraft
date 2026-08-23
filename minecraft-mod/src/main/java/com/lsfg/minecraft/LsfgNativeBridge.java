package com.lsfg.minecraft;

public final class LsfgNativeBridge {

    private static volatile boolean loaded = false;

    private LsfgNativeBridge() {}

    public static synchronized boolean isLoaded() {
        return loaded;
    }

    public static synchronized void setLoaded(boolean state) {
        loaded = state;
    }

    public static native String getNativeVersion();

    public static native int initNativeBackend();

    public static native boolean isPlatformSupported();

    public static native int validateAndExtractShaders(String dllPath, String dllSha256, String cacheDir);

    public static native int probeShaderCache(String cacheDir);

    public static native int getCapabilities();

    public static native void shutdown();
}
