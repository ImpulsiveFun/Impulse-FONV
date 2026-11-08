package org.fnvprobe;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Imports locally supplied files. Possession and format checks do not prove purchase. */
final class GameAssetImport {
    private static final String[] BASE = {"FalloutNV.esm", "Fallout - Meshes.bsa", "Fallout - Misc.bsa",
        "Fallout - Sound.bsa", "Fallout - Textures.bsa", "Fallout - Textures2.bsa", "Fallout - Voices1.bsa"};
    interface Progress { void update(String message); }

    private static String acceptedPath(String name) throws IOException {
        String normalized = name.replace('\\', '/');
        if (normalized.startsWith("/")) throw new IOException("The ZIP contains an absolute path.");
        String[] parts = normalized.split("/");
        for (String part : parts) if (part.equals("..") || part.indexOf(':') >= 0)
            throw new IOException("The ZIP contains an unsafe path.");
        for (int i = 0; i < parts.length; ++i) {
            if (!parts[i].equalsIgnoreCase("Data")) continue;
            if (i + 2 == parts.length) {
                for (String base : BASE) if (parts[i + 1].equalsIgnoreCase(base)) return "Data/" + base;
                String official = GameDlc.canonicalName(parts[i + 1]);
                if (official != null) return "Data/" + official;
                String lower = parts[i + 1].toLowerCase(Locale.ROOT);
                if (lower.endsWith(".bsa") || lower.endsWith(".esm") || lower.endsWith(".esp"))
                    return "Data/" + parts[i + 1];
            }
            if (i + 2 < parts.length && (parts[i + 1].equalsIgnoreCase("Music") ||
                    parts[i + 1].equalsIgnoreCase("Sound") || parts[i + 1].equalsIgnoreCase("Shaders") ||
                    parts[i + 1].equalsIgnoreCase("Video"))) {
                StringBuilder result = new StringBuilder("Data");
                for (int j = i + 1; j < parts.length; ++j) {
                    if (parts[j].isEmpty() || parts[j].equals(".")) throw new IOException("Invalid ZIP path.");
                    result.append('/').append(parts[j]);
                }
                return result.toString();
            }
        }
        String base = parts.length == 0 ? "" : parts[parts.length - 1];
        if (base.equalsIgnoreCase("FalloutNV.exe")) return "FalloutNV.exe";
        if (base.equalsIgnoreCase("Fallout_default.ini")) return "Fallout_default.ini";
        if (base.equalsIgnoreCase("MainTitle.wav")) return "MainTitle.wav";
        return null;
    }
    private static void removeStaging(File directory) throws IOException {
        File[] children = directory.listFiles();
        if (children != null) for (File child : children) {
            if (child.isDirectory()) removeStaging(child);
            else if (!child.delete()) throw new IOException("Could not remove temporary import file.");
        }
        if (directory.exists() && !directory.delete()) throw new IOException("Could not remove temporary import folder.");
    }
    private static void commitFiles(File source, File destination) throws IOException {
        File[] children = source.listFiles();
        if (children == null) throw new IOException("Could not read imported files.");
        if (!destination.isDirectory() && !destination.mkdirs()) throw new IOException("Could not create game folder.");
        for (File child : children) {
            File target = GameDlc.canonicalName(child.getName()) == null
                    ? new File(destination, child.getName()) : GameConfiguration.existingName(destination, child.getName());
            if (child.isDirectory()) commitFiles(child, target);
            else Files.move(child.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
    }
    static String importZip(InputStream source, File privateFiles, String expectedDigest, Progress progress) throws IOException {
        return importZip(source, privateFiles, expectedDigest, progress, false);
    }

    static String importDlcZip(InputStream source, File privateFiles, String expectedDigest, Progress progress) throws IOException {
        return importZip(source, privateFiles, expectedDigest, progress, true);
    }

    private static String importZip(InputStream source, File privateFiles, String expectedDigest, Progress progress, boolean dlc) throws IOException {
        File staging = new File(privateFiles, "import-" + UUID.randomUUID());
        if (!staging.mkdir()) throw new IOException("Could not create temporary import folder.");
        byte[] buffer = new byte[256 * 1024];
        long total = 0;
        int count = 0;
        Set<String> seen = new HashSet<>();
        try (ZipInputStream zip = new ZipInputStream(source)) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                if (entry.isDirectory()) continue;
                String relative = acceptedPath(entry.getName());
                if (relative == null) continue;
                if (dlc && (!relative.startsWith("Data/") || GameDlc.canonicalName(relative.substring(5)) == null)) continue;
                if (!seen.add(relative.toLowerCase(Locale.ROOT))) throw new IOException("Duplicate game file in ZIP: " + relative);
                if (++count > 100000) throw new IOException("The archive contains too many files.");
                File output = new File(staging, relative);
                if (!output.getCanonicalPath().startsWith(staging.getCanonicalPath() + File.separator))
                    throw new IOException("Invalid import destination.");
                File parent = output.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Could not create import subfolder.");
                progress.update("Importing " + relative + "…");
                try (FileOutputStream file = new FileOutputStream(output)) {
                    for (int n; (n = zip.read(buffer)) != -1;) {
                        total += n;
                        if (total > 32L * 1024 * 1024 * 1024) throw new IOException("The game archive exceeds the 32 GiB import limit.");
                        file.write(buffer, 0, n);
                    }
                }
            }
            File game = new File(privateFiles, "game");
            File baseRoot = dlc ? game : staging;
            int packs = dlc ? GameDlc.completeMasters(new File(staging, "Data"), true).size() : 0;
            if (dlc && packs == 0) throw new IOException("Include the add-on ESM and its matching BSA files under Data/ in the ZIP.");
            for (String name : BASE) {
                File file = GameConfiguration.existingName(new File(baseRoot, "Data"), name);
                if (file.length() < 1024 * 1024) throw new IOException("Missing or incomplete base asset: " + name);
                if (!GameDlc.validHeader(file, name.endsWith(".esm"))) throw new IOException("Invalid asset header: " + name);
            }
            File executable = new File(baseRoot, "FalloutNV.exe");
            if (executable.length() == 0 || executable.length() > 32L * 1024 * 1024)
                throw new IOException(dlc ? "Import your base game before importing add-ons."
                        : "The ZIP must include the executable used to build this APK.");
            MessageDigest digest;
            try { digest = MessageDigest.getInstance("SHA-256"); }
            catch (NoSuchAlgorithmException impossible) { throw new IOException(impossible); }
            try (InputStream in = Files.newInputStream(executable.toPath())) {
                for (int n; (n = in.read(buffer)) != -1;) digest.update(buffer, 0, n);
            }
            StringBuilder hash = new StringBuilder(64);
            for (byte b : digest.digest()) hash.append(String.format(Locale.ROOT, "%02x", b & 255));
            if (!hash.toString().equals(expectedDigest)) throw new IOException("This executable differs from the one used to build this APK.");
            // Only imported game files are replaced. Saves and user settings retain their paths.
            if (!game.isDirectory() && !game.mkdirs()) throw new IOException("Could not create game folder.");
            File marker = new File(game, dlc ? GameDlc.IMPORT_MARKER : ".import-in-progress");
            if (dlc) {
                if (marker.exists() && !seen.containsAll(Files.readAllLines(marker.toPath())))
                    throw new IOException("Import the same DLC ZIP again to finish the previous import.");
                Files.write(marker.toPath(), seen, java.nio.charset.StandardCharsets.UTF_8);
            } else {
                try (FileOutputStream state = new FileOutputStream(marker)) { state.write(1); }
            }
            commitFiles(staging, game);
            if (!marker.delete()) throw new IOException("Could not finish the import transaction.");
            return dlc ? "Imported " + packs + " official add-on" + (packs == 1 ? "." : "s.") + " Tap Play to load them."
                    : "Imported " + count + " game files. Choose your controls and tap Play.";
        } finally { removeStaging(staging); }
    }
}
