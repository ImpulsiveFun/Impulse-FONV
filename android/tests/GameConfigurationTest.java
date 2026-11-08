package org.fnvprobe;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public final class GameConfigurationTest {
    private static void check(boolean okay) { if (!okay) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        String fresh = GameConfiguration.setAdapter("", "Test adapter");
        check(fresh.equals("[Display]\nsD3DDevice=\"Test adapter\"\n"));
        check(GameConfiguration.setAdapter(fresh,"Test adapter").equals(fresh));
        String settings = ";custom\r\n[display]\r\niSize W=640\r\nsd3ddevice = old\r\n[Audio]\r\nfVolume=0.4\r\n";
        String updated = GameConfiguration.setAdapter(settings,"Test adapter");
        check(updated.equals(";custom\r\n[display]\r\niSize W=640\r\nsD3DDevice=\"Test adapter\"\r\n[Audio]\r\nfVolume=0.4\r\n"));
        String inserted = GameConfiguration.setAdapter("[Display]\niSize W=640\n[Audio]\nx=1", "Adapter B");
        check(inserted.equals("[Display]\niSize W=640\nsD3DDevice=\"Adapter B\"\n[Audio]\nx=1"));
        String duplicate = GameConfiguration.setAdapter("[Display]\nsD3DDevice=a\nsD3DDevice=b\n", "Adapter C");
        check(duplicate.equals("[Display]\nsD3DDevice=\"Adapter C\"\n"));

        Path root = Files.createTempDirectory("fnv-settings-test-");
        Files.writeString(root.resolve("Fallout_default.ini"),"[General]\nOwnTestSetting=1\n");
        GameConfiguration.prepare(root.toFile(),"Test adapter");
        Path user = root.resolve("UserDocuments/My Games/FalloutNV");
        check(Files.readString(user.resolve("FALLOUT.INI")).contains("OwnTestSetting=1"));
        Files.writeString(user.resolve("FALLOUT.INI"),"[General]\nUserSetting=2\n");
        Files.move(user.resolve("FalloutPrefs.ini"),user.resolve("FALLOUTPREFS.INI"));
        Files.write(user.resolve("FALLOUTPREFS.INI"),settings.getBytes(StandardCharsets.ISO_8859_1));
        GameConfiguration.prepare(root.toFile(),"Test adapter");
        check(!Files.exists(user.resolve("FalloutPrefs.ini")) || user.resolve("FalloutPrefs.ini").toRealPath().equals(user.resolve("FALLOUTPREFS.INI").toRealPath()));
        check(Files.readString(user.resolve("FALLOUT.INI")).contains("UserSetting=2"));
        check(Files.readString(user.resolve("FALLOUTPREFS.INI")).equals(updated));
        GameConfiguration.prepare(root.toFile(),"Replacement adapter");
        check(Files.readString(user.resolve("FALLOUTPREFS.INI")).contains("Replacement adapter"));
        System.out.println("PASS: settings bootstrap, adapter updates, user settings, case handling and repeat runs.");

        Path data = Files.createDirectory(root.resolve("Data"));
        byte[] master = new byte[24], archive = new byte[36];
        System.arraycopy(new byte[]{'T', 'E', 'S', '4'}, 0, master, 0, 4);
        System.arraycopy(new byte[]{'B', 'S', 'A', 0}, 0, archive, 0, 4);
        Files.write(data.resolve("FalloutNV.esm"), master);
        Files.write(data.resolve("deadmoney.esm"), master);
        Files.write(data.resolve("DeadMoney - Main.bsa"), archive);
        Files.write(data.resolve("DeadMoney - Sounds.bsa"), archive);
        Files.write(data.resolve("ClassicPack.esm"), master);
        Files.write(data.resolve("ClassicPack - Main.bsa"), archive);
        Files.write(data.resolve("HonestHearts.esm"), master);
        Path local = Files.createDirectories(root.resolve("LocalAppData/FalloutNV"));
        Path plugins = local.resolve("plugins.txt");
        Files.writeString(plugins, "# preserve comments\nTest.esp\n*HonestHearts.esm\nDeadMoney.esm\n");
        GameConfiguration.prepare(root.toFile(), "Replacement adapter");
        check(Files.readString(plugins).equals("FalloutNV.esm\r\ndeadmoney.esm\r\nClassicPack.esm\r\n# preserve comments\r\nTest.esp\r\n"));
        check(Files.getLastModifiedTime(data.resolve("deadmoney.esm"))
                .compareTo(Files.getLastModifiedTime(data.resolve("FalloutNV.esm"))) > 0);
        check(Files.getLastModifiedTime(data.resolve("ClassicPack.esm"))
                .compareTo(Files.getLastModifiedTime(data.resolve("deadmoney.esm"))) > 0);
        var pluginTime = Files.getLastModifiedTime(plugins);
        GameConfiguration.prepare(root.toFile(), "Replacement adapter");
        check(Files.getLastModifiedTime(plugins).equals(pluginTime));
        System.out.println("PASS: complete official DLC activation, incomplete DLC omission, load order and repeat launch.");
    }
}
