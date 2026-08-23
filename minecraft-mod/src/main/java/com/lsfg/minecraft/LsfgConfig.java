package com.lsfg.minecraft;

import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.nio.file.Path;

public final class LsfgConfig {

    public boolean enabled = true;
    public int multiplier = 2;
    public float flowScale = 1.0f;
    public boolean performanceMode = true;
    public boolean framegenFp16 = true;
    public boolean antiArtifacts = true;

    private static final String CONFIG_FILENAME = "config.json";

    public static LsfgConfig load(Path gameDir) {
        Path configDir = (gameDir != null ? gameDir : new File(".").toPath())
            .resolve("config")
            .resolve("lsfg-minecraft");
        File configFile = configDir.resolve(CONFIG_FILENAME).toFile();

        LsfgConfig config = new LsfgConfig();
        if (configFile.exists() && configFile.isFile()) {
            try (FileReader reader = new FileReader(configFile)) {
                char[] buffer = new char[(int) configFile.length()];
                int read = reader.read(buffer);
                String json = new String(buffer, 0, read);
                if (json.contains("\"enabled\":false")) config.enabled = false;
                if (json.contains("\"multiplier\":3")) config.multiplier = 3;
                if (json.contains("\"performanceMode\":false")) config.performanceMode = false;
                if (json.contains("\"framegenFp16\":false")) config.framegenFp16 = false;
            } catch (Throwable ignored) {}
        } else {
            config.save(gameDir);
        }
        return config;
    }

    public void save(Path gameDir) {
        Path configDir = (gameDir != null ? gameDir : new File(".").toPath())
            .resolve("config")
            .resolve("lsfg-minecraft");
        File dir = configDir.toFile();
        if (!dir.exists()) dir.mkdirs();

        File configFile = new File(dir, CONFIG_FILENAME);
        String json = "{\n" +
            "  \"enabled\": " + enabled + ",\n" +
            "  \"multiplier\": " + multiplier + ",\n" +
            "  \"flowScale\": " + flowScale + ",\n" +
            "  \"performanceMode\": " + performanceMode + ",\n" +
            "  \"framegenFp16\": " + framegenFp16 + ",\n" +
            "  \"antiArtifacts\": " + antiArtifacts + "\n" +
            "}\n";

        try (FileWriter writer = new FileWriter(configFile)) {
            writer.write(json);
            writer.flush();
        } catch (Throwable ignored) {}
    }
}
