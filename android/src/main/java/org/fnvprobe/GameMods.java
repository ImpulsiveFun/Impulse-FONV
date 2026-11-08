package org.fnvprobe;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.FileTime;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.stream.Collectors;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class GameMods {
    private static final Set<String> ASSET_FOLDERS = new HashSet<>(java.util.Arrays.asList(
            "meshes", "textures", "sound", "music", "menus", "video", "shaders", "lodsettings", "distantlod"));
    private static final long IMPORT_LIMIT = 4L * 1024 * 1024 * 1024;

    static final class Plugin {
        final String name;
        final boolean master;
        final List<String> dependencies;

        Plugin(String name, boolean master, List<String> dependencies) {
            this.name = name;
            this.master = master;
            this.dependencies = dependencies;
        }
    }

    static final class Pack {
        final Path directory;
        final List<Plugin> plugins;
        boolean enabled;

        Pack(Path directory, List<Plugin> plugins) {
            this.directory = directory;
            this.plugins = plugins;
        }

        String label() {
            return plugins.stream().map(plugin -> plugin.name).collect(Collectors.joining(", "));
        }
    }

    private static String key(String name) { return name.toLowerCase(Locale.ROOT); }

    private static boolean isPlugin(String name) {
        String lower = key(name);
        return lower.endsWith(".esm") || lower.endsWith(".esp");
    }

    private static String dataPath(String name) throws IOException {
        String[] parts = name.replace('\\', '/').split("/", -1);
        for (String part : parts) {
            if (!part.matches("[ -~]{1,180}") || part.matches(".*[:*?\"<>|].*")
                    || part.startsWith(".") || part.endsWith(".") || part.endsWith(" "))
                throw new IOException("Invalid mod ZIP path: " + name);
            String lower = key(part);
            if (lower.equals("nvse") || lower.endsWith(".dll") || lower.endsWith(".exe"))
                throw new IOException("This mod includes Windows or script extender files: " + name);
        }
        for (int i = 0; i < parts.length - 1; i++) {
            if (parts[i].equalsIgnoreCase("Data")) {
                parts = java.util.Arrays.copyOfRange(parts, i + 1, parts.length);
                break;
            }
        }
        if (parts.length == 1 && (isPlugin(parts[0]) || key(parts[0]).endsWith(".bsa"))) return parts[0];
        if (parts.length > 1 && ASSET_FOLDERS.contains(key(parts[0]))) return String.join("/", parts);
        return null;
    }

    private static Path store(File root) { return root.toPath().resolve("AndroidMods"); }

    static synchronized String importZip(InputStream source, File root, GameAssetImport.Progress progress) throws IOException {
        Path store = Files.createDirectories(store(root));
        Path staged = Files.createTempDirectory(store, ".import-");
        try {
            Set<String> paths = new HashSet<>();
            long total = 0;
            int entries = 0;
            byte[] buffer = new byte[128 * 1024];
            try (ZipInputStream zip = new ZipInputStream(source)) {
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                    if (++entries > 50000) throw new IOException("The mod ZIP has too many entries.");
                    if (entry.isDirectory()) continue;
                    String relative = dataPath(entry.getName());
                    Path output = null;
                    if (relative != null) {
                        if (relative.length() > 700 || !paths.add(key(relative)))
                            throw new IOException("Duplicate or overly long mod path: " + relative);
                        if (GameDlc.canonicalName(relative) != null || key(relative).equals("falloutnv.esm"))
                            throw new IOException("Use the game or DLC importer for " + relative + ".");
                        output = staged.resolve(relative);
                        Files.createDirectories(output.getParent());
                        if (paths.size() == 1 || paths.size() % 100 == 0) progress.update("Importing " + relative + "…");
                    }
                    try (java.io.OutputStream file = output == null ? null : new BufferedOutputStream(Files.newOutputStream(output))) {
                        for (int n; (n = zip.read(buffer)) != -1;) {
                            total += n;
                            if (total > IMPORT_LIMIT) throw new IOException("The mod ZIP exceeds the 4 GiB import limit.");
                            if (file != null) file.write(buffer, 0, n);
                        }
                    }
                    if (output != null && key(relative).endsWith(".bsa") && !GameDlc.validHeader(output.toFile(), false))
                        throw new IOException("Invalid mod archive: " + relative);
                }
            }
            Pack pack = readPack(staged);
            if (pack.plugins.isEmpty()) throw new IOException("Include an ESP or ESM in the ZIP root or a Data folder.");
            Set<String> names = new HashSet<>();
            for (Pack existing : load(root))
                for (Plugin plugin : existing.plugins) names.add(key(plugin.name));
            File data = GameConfiguration.existingName(root, "Data");
            for (Plugin plugin : pack.plugins) {
                if (!names.add(key(plugin.name))) throw new IOException("A mod with " + plugin.name + " is already imported.");
                if (Files.exists(GameConfiguration.existingName(data, plugin.name).toPath(), LinkOption.NOFOLLOW_LINKS))
                    throw new IOException(plugin.name + " is already installed in the game's Data folder.");
            }
            Files.move(staged, store.resolve(UUID.randomUUID().toString()), StandardCopyOption.ATOMIC_MOVE);
            return pack.label();
        } finally {
            if (Files.exists(staged)) {
                try (Stream<Path> paths = Files.walk(staged)) {
                    for (Path path : paths.sorted(Comparator.reverseOrder()).collect(Collectors.toList())) Files.delete(path);
                }
            }
        }
    }

    private static Pack readPack(Path directory) throws IOException {
        List<Plugin> plugins = new ArrayList<>();
        try (Stream<Path> files = Files.list(directory)) {
            for (Path path : files.sorted().collect(Collectors.toList()))
                if (Files.isRegularFile(path) && isPlugin(path.getFileName().toString())) plugins.add(readPlugin(path));
        }
        return new Pack(directory, plugins);
    }

    private static byte[] readBytes(InputStream in, int size) throws IOException {
        byte[] bytes = new byte[size];
        new java.io.DataInputStream(in).readFully(bytes);
        return bytes;
    }

    static Plugin readPlugin(Path path) throws IOException {
        String name = path.getFileName().toString();
        try (InputStream in = Files.newInputStream(path)) {
            ByteBuffer header = ByteBuffer.wrap(readBytes(in, 24)).order(ByteOrder.LITTLE_ENDIAN);
            if (header.getInt() != 0x34534554) throw new IOException("Invalid plugin header.");
            int size = header.getInt();
            int flags = header.getInt();
            if (size < 0 || size > 1024 * 1024 || (flags & 0x40000) != 0)
                throw new IOException("Unsupported plugin header.");
            ByteBuffer records = ByteBuffer.wrap(readBytes(in, size)).order(ByteOrder.LITTLE_ENDIAN);
            List<String> dependencies = new ArrayList<>();
            while (records.hasRemaining()) {
                int type = records.getInt();
                int length = Short.toUnsignedInt(records.getShort());
                if (length > records.remaining()) throw new IOException("Invalid plugin field.");
                if (type == 0x5453414d) {
                    if (length < 2 || records.get(records.position() + length - 1) != 0)
                        throw new IOException("Invalid master name.");
                    byte[] master = new byte[length - 1];
                    records.get(master);
                    String dependency = new String(master, StandardCharsets.ISO_8859_1);
                    if (!isPlugin(dependency)) throw new IOException("Invalid master name.");
                    dependencies.add(dependency);
                    records.get();
                } else {
                    records.position(records.position() + length);
                }
            }
            return new Plugin(name, (flags & 1) != 0 || key(name).endsWith(".esm"), dependencies);
        } catch (IOException | java.nio.BufferUnderflowException error) {
            throw new IOException("Could not read " + name + ": " + error.getMessage(), error);
        }
    }

    static synchronized List<Pack> load(File root) throws IOException {
        Path store = store(root);
        if (!Files.isDirectory(store)) return new ArrayList<>();
        Map<String, Pack> available = new LinkedHashMap<>();
        try (Stream<Path> paths = Files.list(store)) {
            for (Path path : paths.sorted().collect(Collectors.toList())) {
                String id = path.getFileName().toString();
                if (id.matches("[0-9a-f-]{36}") && Files.isDirectory(path)) available.put(id, readPack(path));
            }
        }
        List<Pack> packs = new ArrayList<>();
        Path state = store.resolve("order.txt");
        if (Files.isRegularFile(state)) {
            for (String line : Files.readAllLines(state, StandardCharsets.UTF_8)) {
                if (line.length() != 37) continue;
                Pack pack = available.remove(line.substring(1));
                if (pack == null) continue;
                pack.enabled = line.charAt(0) == '+';
                packs.add(pack);
            }
        }
        packs.addAll(available.values());
        return packs;
    }

    static synchronized void save(File root, List<Pack> packs) throws IOException {
        List<String> order = new ArrayList<>();
        for (Pack pack : packs) order.add((pack.enabled ? "+" : "-") + pack.directory.getFileName());
        writeList(store(root).resolve("order.txt"), order);
    }

    private static void writeList(Path target, List<String> lines) throws IOException {
        Path staged = Files.createTempFile(target.getParent(), ".state-", ".tmp");
        try {
            Files.write(staged, lines, StandardCharsets.UTF_8);
            Files.move(staged, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } finally { Files.deleteIfExists(staged); }
    }

    private static Path dataFile(Path data, String relative) {
        File target = data.toFile();
        for (String part : relative.split("/")) target = GameConfiguration.existingName(target, part);
        return target.toPath();
    }

    private static boolean modLink(Path path, Path store) throws IOException {
        return Files.isSymbolicLink(path) && Files.readSymbolicLink(path).startsWith(store.toAbsolutePath());
    }

    static synchronized List<String> deploy(File root, Path data, Set<String> available, Set<String> managed,
                                             long lastOfficialTime) throws IOException {
        Path store = store(root);
        if (!Files.isDirectory(store)) return Collections.emptyList();
        List<Plugin> ordered = new ArrayList<>();
        Map<String, Path> files = new LinkedHashMap<>();
        Map<String, String> spellings = new LinkedHashMap<>();
        for (Pack pack : load(root)) {
            for (Plugin plugin : pack.plugins) managed.add(key(plugin.name));
            if (!pack.enabled) continue;
            List<Plugin> pending = new ArrayList<>(pack.plugins);
            while (!pending.isEmpty()) {
                int previousSize = pending.size();
                for (java.util.Iterator<Plugin> it = pending.iterator(); it.hasNext();) {
                    Plugin plugin = it.next();
                    boolean waiting = pending.stream().anyMatch(other ->
                            plugin.dependencies.stream().anyMatch(other.name::equalsIgnoreCase));
                    if (waiting) continue;
                    ordered.add(plugin);
                    it.remove();
                }
                if (pending.size() == previousSize)
                    throw new IOException("Circular plugin dependencies in " + pack.label() + ".");
            }
            try (Stream<Path> paths = Files.walk(pack.directory)) {
                for (Path path : paths.filter(Files::isRegularFile).collect(Collectors.toList())) {
                    String relative = pack.directory.relativize(path).toString().replace(File.separatorChar, '/');
                    files.put(key(relative), path);
                    spellings.put(key(relative), relative);
                }
            }
        }
        // New Vegas groups masters before regular plugins. Keep the chosen order within each group.
        ordered.sort(Comparator.comparing(plugin -> !plugin.master));
        List<String> names = new ArrayList<>();
        for (Plugin plugin : ordered) {
            for (String dependency : plugin.dependencies) {
                if (!available.contains(key(dependency)))
                    throw new IOException(plugin.name + " needs " + dependency + " enabled earlier in the load order.");
            }
            available.add(key(plugin.name));
            names.add(plugin.name);
        }
        Path manifest = store.resolve("deployed.txt");
        Set<String> previous = Files.exists(manifest)
                ? new HashSet<>(Files.readAllLines(manifest, StandardCharsets.UTF_8)) : new HashSet<>();
        Map<Path, Path> desired = new LinkedHashMap<>();
        for (Map.Entry<String, Path> item : files.entrySet()) {
            Path target = dataFile(data, spellings.get(item.getKey()));
            if (Files.exists(target, LinkOption.NOFOLLOW_LINKS) && !modLink(target, store))
                throw new IOException("A mod would replace an existing game file: " + item.getKey());
            desired.put(target, item.getValue().toAbsolutePath());
        }
        Set<String> touched = new HashSet<>(previous);
        touched.addAll(files.keySet());
        writeList(manifest, new ArrayList<>(touched));
        for (String relative : previous) {
            Path target = dataFile(data, relative);
            if (!files.containsKey(relative) && modLink(target, store)) Files.delete(target);
        }
        for (Map.Entry<Path, Path> item : desired.entrySet()) {
            Path target = dataFile(data, data.relativize(item.getKey()).toString().replace(File.separatorChar, '/'));
            if (modLink(target, store)) {
                if (Files.readSymbolicLink(target).equals(item.getValue())) continue;
                Files.delete(target);
            }
            Files.createDirectories(target.getParent());
            Files.createSymbolicLink(target, item.getValue());
        }
        writeList(manifest, new ArrayList<>(files.keySet()));
        for (String name : names) {
            lastOfficialTime += 2000;
            Path path = files.get(key(name));
            if (Files.getLastModifiedTime(path).toMillis() != lastOfficialTime)
                Files.setLastModifiedTime(path, FileTime.fromMillis(lastOfficialTime));
        }
        return names;
    }
}
