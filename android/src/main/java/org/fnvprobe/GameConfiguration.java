package org.fnvprobe;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/** Supplies the adapter setting normally saved by the Windows launcher. */
final class GameConfiguration {
    static File existingName(File parent, String name) {
        File[] files = parent.listFiles();
        if (files != null) for (File file : files)
            if (file.getName().equalsIgnoreCase(name)) return file;
        return new File(parent, name);
    }

    static String setAdapter(String original, String adapter) {
        String ending = original.contains("\r\n") ? "\r\n" : "\n";
        String[] lines = original.split("(?<=\n)", -1);
        StringBuilder result = new StringBuilder();
        boolean display = false, found = false, sawDisplay = false;
        for (String line : lines) {
            String trimmed = line.trim();
            if (trimmed.startsWith("[")) {
                if (display && !found) {
                    if (result.length() > 0 && result.charAt(result.length()-1) != '\n') result.append(ending);
                    result.append("sD3DDevice=\"").append(adapter).append('"').append(ending);
                    found = true;
                }
                int close = trimmed.indexOf(']');
                display = close > 0 && trimmed.substring(1, close).trim().equalsIgnoreCase("Display");
                sawDisplay |= display;
            }
            int equal = trimmed.indexOf('=');
            if (display && equal >= 0 && trimmed.substring(0,equal).trim().equalsIgnoreCase("sD3DDevice")) {
                if (!found) result.append("sD3DDevice=\"").append(adapter).append('"').append(ending);
                found = true;
            } else result.append(line);
        }
        if (!found) {
            if (result.length() > 0 && result.charAt(result.length()-1) != '\n') result.append(ending);
            if (!sawDisplay || !display) result.append("[Display]").append(ending);
            result.append("sD3DDevice=\"").append(adapter).append('"').append(ending);
        }
        return result.toString();
    }

    static synchronized void prepare(File root, String adapter) throws IOException {
        GameDlc.prepare(root);
        File user = new File(root, "UserDocuments/My Games/FalloutNV");
        if (!user.isDirectory() && !user.mkdirs()) throw new IOException("Could not create game settings folder.");
        File main = existingName(user, "FALLOUT.INI");
        File defaults = existingName(root, "Fallout_default.ini");
        if (!main.exists() && defaults.isFile()) Files.copy(defaults.toPath(), main.toPath());
        File prefs = existingName(user, "FalloutPrefs.ini");
        // Preserve the bytes of existing Windows text settings, including comments.
        String original = prefs.isFile() ? new String(Files.readAllBytes(prefs.toPath()), StandardCharsets.ISO_8859_1) : "";
        String updated = setAdapter(original, adapter);
        if (updated.equals(original)) return;
        File temporary = File.createTempFile("adapter-", ".tmp", user);
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
