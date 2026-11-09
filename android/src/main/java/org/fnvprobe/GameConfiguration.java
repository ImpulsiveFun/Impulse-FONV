package org.fnvprobe;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/** Supplies the display settings normally saved by the Windows launcher. */
final class GameConfiguration {
    private static final String GRASS_DISTANCE = "fGrassStartFadeDistance";
    private static final String OBJECT_DISTANCE = "fLODFadeOutMultObjects";
    static final String STANDARD_GRASS = "3500";
    static final String STANDARD_OBJECTS = "5";
    static final String PERFORMANCE_GRASS = "900";
    static final String PERFORMANCE_OBJECTS = "2";
    enum Resolution {
        LOW(640, 480), MEDIUM(800, 600), WIDE(1280, 720), FULL_HD(1920, 1080);

        final int width, height;
        Resolution(int width, int height) {
            this.width = width;
            this.height = height;
        }
        String label() { return width + " × " + height; }

        static Resolution suggested(long totalMemory, int shortEdge, boolean lowRam) {
            // Screen size and memory are a starting estimate, not a GPU benchmark.
            return !lowRam && totalMemory >= 7L * 1024 * 1024 * 1024 && shortEdge >= 720 ? WIDE : LOW;
        }
    }

    static File existingName(File parent, String name) {
        File[] files = parent.listFiles();
        if (files != null) for (File file : files)
            if (file.getName().equalsIgnoreCase(name)) return file;
        return new File(parent, name);
    }

    static String setAdapter(String original, String adapter) {
        return setSetting(original, "Display", "sD3DDevice", "\"" + adapter + "\"");
    }

    static String setResolution(String original, Resolution resolution) {
        String updated = setSetting(original, "Display", "iSize W", Integer.toString(resolution.width));
        return setSetting(updated, "Display", "iSize H", Integer.toString(resolution.height));
    }

    private static boolean isSection(String line, String section) {
        int close = line.indexOf(']');
        return close > 0 && line.substring(1, close).trim().equalsIgnoreCase(section);
    }

    private static String setSetting(String original, String section, String key, String value) {
        String ending = original.contains("\r\n") ? "\r\n" : "\n";
        String setting = key + "=" + value + ending;
        String[] lines = original.split("(?<=\n)", -1);
        StringBuilder result = new StringBuilder();
        boolean inSection = false, found = false;
        for (String line : lines) {
            String trimmed = line.trim();
            if (trimmed.startsWith("[")) {
                if (inSection && !found) {
                    if (result.length() > 0 && result.charAt(result.length()-1) != '\n') result.append(ending);
                    result.append(setting);
                    found = true;
                }
                inSection = isSection(trimmed, section);
            }
            int equal = trimmed.indexOf('=');
            if (inSection && equal >= 0 && trimmed.substring(0,equal).trim().equalsIgnoreCase(key)) {
                if (!found) result.append(setting);
                found = true;
            } else result.append(line);
        }
        if (!found) {
            if (result.length() > 0 && result.charAt(result.length()-1) != '\n') result.append(ending);
            if (!inSection) result.append('[').append(section).append(']').append(ending);
            result.append(setting);
        }
        return result.toString();
    }

    private static String getSetting(String original, String section, String key, String fallback) {
        boolean inSection = false;
        for (String line : original.split("\n")) {
            String text = line.trim();
            if (text.startsWith("[")) inSection = isSection(text, section);
            int equal = text.indexOf('=');
            if (inSection && equal >= 0 && text.substring(0, equal).trim().equalsIgnoreCase(key)) {
                return text.substring(equal + 1).split("[;#]", 2)[0].trim();
            }
        }
        return fallback;
    }

    static synchronized String[] exteriorDetail(File root) throws IOException {
        String original = readPrefs(preferencesFile(root));
        return new String[] {getSetting(original, "Grass", GRASS_DISTANCE, STANDARD_GRASS),
                             getSetting(original, "LOD", OBJECT_DISTANCE, STANDARD_OBJECTS)};
    }

    static synchronized void saveExteriorDetail(File root, String grass, String objects) throws IOException {
        File prefs = preferencesFile(root);
        String original = readPrefs(prefs);
        String updated = setSetting(original, "Grass", GRASS_DISTANCE, grass);
        updated = setSetting(updated, "LOD", OBJECT_DISTANCE, objects);
        writePrefs(prefs, original, updated);
    }

    private static File preferencesFile(File root) {
        return existingName(new File(root, "UserDocuments/My Games/FalloutNV"), "FalloutPrefs.ini");
    }

    private static String readPrefs(File prefs) throws IOException {
        return prefs.isFile() ? new String(Files.readAllBytes(prefs.toPath()), StandardCharsets.ISO_8859_1) : "";
    }

    static void prepare(File root, String adapter) throws IOException {
        prepare(root, adapter, null);
    }

    static synchronized void prepare(File root, String adapter, Resolution resolution) throws IOException {
        GameDlc.prepare(root);
        File user = new File(root, "UserDocuments/My Games/FalloutNV");
        if (!user.isDirectory() && !user.mkdirs()) throw new IOException("Could not create game settings folder.");
        File main = existingName(user, "FALLOUT.INI");
        File defaults = existingName(root, "Fallout_default.ini");
        if (!main.exists() && defaults.isFile()) Files.copy(defaults.toPath(), main.toPath());
        File prefs = preferencesFile(root);
        // Preserve the bytes of existing Windows text settings, including comments.
        String original = readPrefs(prefs);
        String updated = setAdapter(original, adapter);
        if (resolution != null) updated = setResolution(updated, resolution);
        writePrefs(prefs, original, updated);
    }

    private static void writePrefs(File prefs, String original, String updated) throws IOException {
        if (updated.equals(original)) return;
        File user = prefs.getParentFile();
        if (!user.isDirectory() && !user.mkdirs()) throw new IOException("Could not create game settings folder.");
        File temporary = File.createTempFile("prefs-", ".tmp", user);
        try {
            Files.write(temporary.toPath(), updated.getBytes(StandardCharsets.ISO_8859_1));
            try {
                Files.move(temporary.toPath(), prefs.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException unsupported) {
                Files.move(temporary.toPath(), prefs.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally { Files.deleteIfExists(temporary.toPath()); }
    }
}
