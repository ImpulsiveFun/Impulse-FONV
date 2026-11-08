package org.fnvprobe;

import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.FileTime;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Official New Vegas add-ons supplied by the player's PC installation. */
final class GameDlc {
    private static final String[] PACKS = {"DeadMoney", "HonestHearts", "OldWorldBlues",
        "LonesomeRoad", "GunRunnersArsenal", "CaravanPack", "ClassicPack", "MercenaryPack", "TribalPack"};
    static final String IMPORT_MARKER = ".dlc-import-in-progress";

    private static String[] files(int index) {
        String pack = PACKS[index];
        return index < 5 ? new String[]{pack + ".esm", pack + " - Main.bsa", pack + " - Sounds.bsa"}
                : new String[]{pack + ".esm", pack + " - Main.bsa"};
    }

    static String canonicalName(String name) {
        for (int i = 0; i < PACKS.length; i++)
            for (String expected : files(i))
                if (expected.equalsIgnoreCase(name)) return expected;
        return null;
    }

    static List<File> completeMasters(File data, boolean strict) throws IOException {
        List<File> masters = new ArrayList<>();
        for (int i = 0; i < PACKS.length; i++) {
            String[] names = files(i);
            boolean present = false;
            String invalid = null;
            File master = GameConfiguration.existingName(data, names[0]);
            for (String name : names) {
                File file = GameConfiguration.existingName(data, name);
                present |= file.exists();
                boolean plugin = name.endsWith(".esm");
                if (validHeader(file, plugin) || invalid != null) continue;
                invalid = name;
            }
            if (invalid == null) masters.add(master);
            else if (present && strict) throw new IOException("Missing or invalid add-on file: " + invalid);
        }
        return masters;
    }

    static boolean validHeader(File file, boolean plugin) throws IOException {
        if (!file.isFile() || file.length() < (plugin ? 24 : 36)) return false;
        byte[] header = new byte[4];
        try (DataInputStream in = new DataInputStream(Files.newInputStream(file.toPath()))) {
            in.readFully(header);
        }
        return Arrays.equals(header, plugin ? new byte[]{'T','E','S','4'} : new byte[]{'B','S','A',0});
    }

    static void prepare(File root) throws IOException {
        if (new File(root, IMPORT_MARKER).exists())
            throw new IOException("The last add-on import was interrupted. Import the same DLC ZIP again before playing.");
        File data = GameConfiguration.existingName(root, "Data");
        List<File> masters = completeMasters(data, false);
        File local = new File(root, "LocalAppData/FalloutNV");
        File list = GameConfiguration.existingName(local, "plugins.txt");
        File base = GameConfiguration.existingName(data, "FalloutNV.esm");
        if (!base.isFile()) return;
        Set<String> available = new HashSet<>(), managed = new HashSet<>();
        available.add("falloutnv.esm");
        long time = base.lastModified();
        StringBuilder active = new StringBuilder("FalloutNV.esm\r\n");
        for (File master : masters) {
            time += 2000; // The original engine orders masters by file timestamp.
            if (master.lastModified() != time) Files.setLastModifiedTime(master.toPath(), FileTime.fromMillis(time));
            active.append(master.getName()).append("\r\n");
            available.add(master.getName().toLowerCase(Locale.ROOT));
        }
        List<String> mods = GameMods.deploy(root, data.toPath(), available, managed, time);
        if (masters.isEmpty() && mods.isEmpty() && !list.isFile()) return;
        for (String mod : mods) active.append(mod).append("\r\n");
        if (list.isFile()) {
            for (String line : Files.readAllLines(list.toPath(), StandardCharsets.ISO_8859_1)) {
                String name = line.trim();
                if (name.startsWith("*")) name = name.substring(1);
                if (!name.isEmpty() && !name.equalsIgnoreCase("FalloutNV.esm") && canonicalName(name) == null
                        && !managed.contains(name.toLowerCase(Locale.ROOT)))
                    active.append(line).append("\r\n");
            }
            if (new String(Files.readAllBytes(list.toPath()), StandardCharsets.ISO_8859_1).contentEquals(active)) return;
        }
        if (!local.isDirectory() && !local.mkdirs()) throw new IOException("Could not create the add-on settings folder.");
        File temporary = File.createTempFile("plugins-", ".tmp", local);
        try {
            Files.write(temporary.toPath(), active.toString().getBytes(StandardCharsets.ISO_8859_1));
            Files.move(temporary.toPath(), list.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temporary.toPath()); }
    }
}
