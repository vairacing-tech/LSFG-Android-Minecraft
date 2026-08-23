package com.lsfg.minecraft;

import net.fabricmc.api.ModInitializer;
import net.fabricmc.loader.api.FabricLoader;

import java.io.File;
import java.nio.file.Path;

public class LsfgMod implements ModInitializer {

    public static final String MOD_ID = "lsfg-minecraft";
    public static final String MOD_NAME = "LSFG Minecraft";
    public static final String MOD_VERSION = "0.1.0";

    private static LsfgConfig config;
    private static boolean active = false;

    @Override
    public void onInitialize() {
        System.out.println("==================================================================");
        System.out.println("  " + MOD_NAME + " v" + MOD_VERSION + " - Low-Latency Frame Generation");
        System.out.println("==================================================================");
        System.out.println("[LSFG] Fabric mod initialized");

        Path gameDir = resolveGameDir();
        System.out.println("[LSFG] Game directory: " + (gameDir != null ? gameDir.toAbsolutePath() : "<null>"));
        System.out.println("[LSFG] Platform: " + LsfgPlatform.getOs() + " " + LsfgPlatform.getArch());

        config = LsfgConfig.load(gameDir);

        if (!LsfgPlatform.isSupportedPlatform()) {
            System.out.println("[LSFG] Native Frame Generation backend unsupported on this platform (" +
                LsfgPlatform.getOs() + "/" + LsfgPlatform.getArch() + "). Frame Generation disabled.");
            return;
        }

        // 1. Locate and inspect mods/Lossless.dll
        LosslessDllResolver.ResolutionResult dllResult = LosslessDllResolver.resolve(gameDir);
        if (dllResult.getStatus() == LosslessDllResolver.DllStatus.ABSENT) {
            System.out.println("[LSFG] Lossless.dll: NOT FOUND (" + dllResult.getMessage() + ")");
            System.out.println("[LSFG] Lossless.dll not found in mods/. Frame Generation disabled.");
            return;
        }

        if (!dllResult.isValid()) {
            System.out.println("[LSFG] Lossless.dll: INVALID (" + dllResult.getMessage() + ")");
            System.out.println("[LSFG] Lossless.dll found but required LSFG resources are incompatible.");
            return;
        }

        System.out.println("[LSFG] Lossless.dll: FOUND (SHA-256: " + dllResult.getSha256().substring(0, 16) + "...)");

        // 2. Load Native ARM64 backend
        boolean nativeLoaded = LsfgNativeLoader.load();
        if (!nativeLoaded) {
            System.err.println("[LSFG] Warning: Native backend could not be loaded. LSFG disabled.");
            return;
        }

        // 3. Ensure Shader Cache
        ShaderCacheManager.CacheResult cacheResult = ShaderCacheManager.ensureShaderCache(
            gameDir,
            dllResult.getFile(),
            dllResult.getSha256()
        );

        if (!cacheResult.isReady()) {
            System.err.println("[LSFG] Error: Shader cache could not be initialized: " + cacheResult.getMessage());
            return;
        }

        System.out.println("[LSFG] Shader cache: " + cacheResult.getStatus() + " (" + cacheResult.getCacheDir().getAbsolutePath() + ")");

        // 4. Initialize backend state & passive Vulkan observation
        int initCode = LsfgNativeBridge.initNativeBackend();
        if (initCode != 0) {
            System.err.println("[LSFG] Warning: Native backend initialization returned code: " + initCode);
            return;
        }

        active = true;
        System.out.println("[LSFG] Passive diagnostics and backend armed. Ready for Minecraft/Zink presentation.");
    }

    public static boolean isActive() {
        return active;
    }

    public static LsfgConfig getConfig() {
        return config;
    }

    private static Path resolveGameDir() {
        try {
            return FabricLoader.getInstance().getGameDir();
        } catch (Throwable t) {
            return new File(".").toPath();
        }
    }
}
