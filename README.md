# New Vegas Recomp for Android

An independent Android ARM64 port of Fallout: New Vegas built through static recompilation. The project takes inspiration from Fallout-NV-MacOS and includes an Android runtime, launcher, graphics and audio adapters, touch controls, and Bluetooth controller support. [Fallout-NV-MacOS](https://github.com/thetheoryofR/Fallout-NV-MacOS)

**Bring your own compatible PC copy.** You build the playable APK on Windows and import the game data from your installation on Android. The public downloads contain source and an optional setup checker.

| File | Purpose |
| --- | --- |
| Repository source | Build tools, Android runtime, launcher, and original artwork |
| Public **New Vegas Windows Builder** EXE | Builds the playable APK and import ZIP from your local game installation |
| Public **New Vegas Recomp Setup** APK | Checks whether your phone reports ARM64 and OpenGL ES 3.1 support, and can compare your local executable to the tested build |
| Private **New-Vegas-Playable.apk** | The app you build from your own executable and install to play |
| Private **NewVegas-Private.zip** | The game data you copy to your phone and import into the playable app |

## What you need

- **On Windows** you need Windows 10 or 11 (64-bit), an installed PC copy of Fallout: New Vegas, free disk space for the build tools and output, and internet access for the first build.
- **On Android** you need an ARM64 device running Android 10 or later with OpenGL ES 3.1. Allow space for the ZIP and its extracted game data at the same time. The full import accepts up to 32 GiB of extracted files.
- **For the executable** use the same unencrypted 32-bit Windows executable for both the build and the ZIP. The tested 1.4.0.525 executable has SHA-256 `19406942e48724d797300c4ea6be9ac69a32f670b8c35db09279a2258422739f`. Other executable hashes are unverified. File checks identify a format and a matching build. They do not verify a purchase.

## Build the APK on Windows

1. Download and open **New-Vegas-Windows-Builder-v2.0.exe**.
2. Choose the folder containing your PC game's `Data/` folder, then choose a private output folder.
3. Click **Build APK and ZIP**. The builder downloads and caches missing tools. If it needs to set up the Android SDK, review the license in the builder.
4. When it finishes, click **Open output folder** to find the playable APK and import ZIP.

Use the **Tools** tab to select a separate compatible executable, reuse an existing ZIP, or build offline after a successful online build.

The phone can stay disconnected while the builder runs.

### What the builder does

1. **Translate the game locally.** Ghidra analyzes your x86 executable and exports an intermediate representation. The recompiler turns that representation into C and records the original executable's SHA-256. The generated files go into the private output directory.
2. **Build and sign the app.** Gradle and the Android NDK compile the translated code and the Android runtime for ARM64. The builder saves the locally debug-signed app as `New-Vegas-Playable.apk`.
3. **Package the data separately.** The builder creates `NewVegas-Private.zip` from the installed game, including any official DLC it finds. It stores the executable in the archive as `FalloutNV.exe` even if you supplied it from a separate path.

Keep your signing key so future APKs can update the app in place. [Advanced build guide and troubleshooting](BUILD.md)

## Import the game on Android

1. Copy **New-Vegas-Playable.apk** and **NewVegas-Private.zip** to the phone over USB in File Transfer mode.
2. Open the APK on the phone to install **New Vegas Recomp**.
3. Open the playable app, scroll to **Game Content**, choose **Import game files (ZIP)**, and select the private ZIP. Keep the app open until the import completes.
4. Choose **Touch controls** or **Controller only**, then tap **Play**. Select **Continue** in the game menu to resume an existing save.

If you're updating the same app with a matching signing key and your game files are already imported, transfer only the APK.

### What goes in the base ZIP

The Windows builder assembles the expected layout. If you make the archive manually, use these paths.

```text
FalloutNV.exe
Fallout_default.ini
Data/
  FalloutNV.esm
  Fallout - Meshes.bsa
  Fallout - Misc.bsa
  Fallout - Sound.bsa
  Fallout - Textures.bsa
  Fallout - Textures2.bsa
  Fallout - Voices1.bsa
```

The executable and the seven base files under `Data/` are required for import. Include `Fallout_default.ini` from your installation for the normal settings setup. Keep loose game files from the installed `Data/Music/`, `Data/Sound/`, `Data/Shaders/`, and `Data/Video/` folders in their original paths. The Windows script includes those folders when present.

### What happens during import

The app reads the ZIP through Android's file picker and extracts recognized entries into a temporary folder in its private storage. It rejects unsafe paths and duplicate game files, checks that the required ESM and BSA files are present and have valid headers, and compares the executable's SHA-256 to the one recorded when the APK was built. Once validation passes, the app moves the staged files into its private game folder. Subsequent launches use that imported copy. Reimporting replaces game files and preserves the separate save and settings folders.

If you see **“This executable differs from the one used to build this APK”**, rebuild the APK and ZIP from the same executable. For a **missing or incomplete base asset** error, check that the archive includes the original file under `Data/`. If an import was interrupted, select the same ZIP again to finish it.

### DLC and mods

- **Official DLC** can go in the base ZIP or be added later with **Game Content → Import DLC (ZIP)**. Put each DLC's `.esm` and matching ` - Main.bsa` under `Data/`. The four story DLCs and Gun Runners' Arsenal also need their matching ` - Sounds.bsa`. The launcher activates complete official sets when you tap **Play**.
- **Mods** use **Game Content → Mods → Import ZIP**. Put the `.esp` or `.esm` at the ZIP root or in `Data/`, alongside any supported assets. Check the mod to enable it, use **Up** and **Down** to change priority, then tap **Done**. The app checks for required masters before launch and applies the load order when you tap **Play**.
- **xNVSE** compatibility currently covers `GetBaseObject`, verified with a test quest. Mods that need other xNVSE commands, JIP LN, or Windows DLL plugins require additional support.

## What changed for Android

| Area | Android work |
| --- | --- |
| Recompilation and startup | ARM64 ahead-of-time build with Android replacements for Windows file access, memory, threads, synchronization, timing, and other startup services. |
| Graphics | Direct3D 9 rendering through OpenGL ES, including runtime shader translation to GLSL, texture handling, and support for occlusion queries. |
| Performance and stability | Cached shader uniform locations and graphics state, reduced redundant uploads and state changes, and worked through renderer stops encountered during outdoor play. Frame rate and stutter vary by device and scene. |
| Audio | DirectSound PCM mixing through AAudio, Vorbis and media playback adapters, and a WAV-loading fix for weapon sounds. |
| Touch controls | Virtual movement stick and swipe aiming, dedicated gameplay buttons, a movable and resizable layout, and touch aim sensitivity separate from the controller. With aim held, tapping the movement stick fires. **View** switches between first and third person. |
| Controllers | Android Bluetooth gamepad input presented to the game through an Xbox-style interface, adjustable right-stick sensitivity, and a controller-only mode that hides touch controls. |
| Play and saves | Full-screen play without the earlier 90-second probe limit, plus normal in-game saving and **Continue**. |
| Content | Official DLC detection and load order, ZIP-based mod import with enable and disable controls, and the first xNVSE scripting command. |
| Presentation | Mojave sunset artwork, an original adaptive road-emblem icon, and a launcher that groups **Play**, **Controls**, and **Game Content**. |

## Tested scope

Testing focused on one ARM64 Samsung foldable. Device tests covered loading and resuming a Goodsprings save, touch and Bluetooth input, third-person switching, and the opening quests for all four story DLCs. Sierra Madre Villa, Zion Canyon, Big MT, and Hopeville rendered after console travel. Two conflicting test mods verified load order and disabling, and the xNVSE test quest verified `GetBaseObject` on a copy of a save. Full campaign completion, normal DLC entrance sequences, broader device coverage, and consistent outdoor 30 FPS are unverified. The Android runtime currently skips Bink video playback.

When reporting an issue, include the app version, phone model, Android version, control mode, location in the game, and steps to reproduce. Share relevant error text with personal paths removed.

## Credits and distribution

The repository contains source, documentation, original launcher art, synthetic test generators, and license notices. Keep the original game executable, assets, DLC, saves, translated C, and personal playable APK private. GitHub releases can offer the separate setup checker.

Fallout and Fallout: New Vegas identify the original game and belong to their respective rights holders. This project is unaffiliated with the game's publishers and developers. [Upstream MIT license](LICENSE) · [Credits and dependency notices](NOTICES.md)

