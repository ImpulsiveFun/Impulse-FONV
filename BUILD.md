# Build and install locally

Use an ARM64 phone running Android 10 or later with OpenGL ES 3.1. The setup APK can report those capabilities before you build. It runs separately and the game never needs it.

## Windows build for later transfer

Open the standalone **New-Vegas-Windows-Builder-v2.2.exe** on 64-bit Windows 10 or 11. Pick the PC game folder and a private output folder, then click **Build APK and ZIP**. The builder extracts its source and caches missing tools under `%LOCALAPPDATA%\NewVegasBuilder`. Android SDK setup asks you to review the license. A first build needs internet access and can take a while.

The animated bar and stage label stay visible during long steps such as installing tools, translating the executable, compiling the APK, and creating the ZIP. The bar fills when the build succeeds and clears if the build stops.

The build needs a compatible 32-bit Windows executable with unencrypted game code. If your Steam copy uses SteamStub DRM, use Steamless by atom0s on your own `FalloutNV.exe`. To use the default build path, back up the original EXE outside the game folder, rename the unpacked EXE to exactly `FalloutNV.exe`, and replace the original next to `Data`. You can also keep the installation untouched by selecting the unpacked file under **Tools → Separate EXE (optional)** or passing its path to `-Executable`. Use that same executable for the APK and import ZIP. [Steamless by atom0s](https://github.com/atom0s/Steamless)

The **Tools** tab lets you use an alternate compatible game executable or supply your own tool paths. Leave **Use cached dependencies only** off for the first build with each builder version. The builder checks the audio dependencies before translating the game. After a successful online build with that version, you can select cached-only mode for later builds.

To compile the standalone builder from the repository, run `windows/Build-Launcher.ps1 -OutputFile .\New-Vegas-Windows-Builder.exe` on Windows with the 64-bit .NET Framework 4.x compiler. The script embeds the build source in the EXE.

### PowerShell alternative

For a command-line build, install the following tools and run the commands from the repository root.

- Python 3.12 or later.
- Ghidra 12.2 with its required Java version. The development build used JDK 25 for Ghidra.
- Gradle 8.9, JDK 21 for Gradle, and Android SDK platform 35. Install Android Build Tools `34.0.0`, NDK `27.2.12479018`, and CMake `3.22.1` through Android Studio's SDK Manager. Install Android Platform-Tools if you use ADB.

Keep your game files and generated build input outside the repository. The tested input is an unencrypted 32-bit Windows Fallout: New Vegas 1.4.0.525 executable with SHA-256 `19406942e48724d797300c4ea6be9ac69a32f670b8c35db09279a2258422739f`. File checks identify a format or a known build. They do not verify a purchase. Compatibility of executables with a different hash is unverified.

Use the paths from your own PC.

```powershell
.\Build-Windows.ps1 `
  -GamePath 'C:\Games\Fallout New Vegas' `
  -OutputPath 'C:\NewVegasLocal' `
  -GhidraHeadless 'C:\Tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat' `
  -GhidraJavaHome 'C:\Tools\jdk-25' `
  -GradleJavaHome 'C:\Program Files\Java\jdk-21'
```

The script creates **New-Vegas-Playable.apk** and **NewVegas-Private.zip** in your output folder. A first build needs access to the build dependencies. You can run later builds with `-Offline` once those dependencies are cached. If Gradle or Python is outside your PATH, pass the full command path with `-GradleCommand` or `-PythonCommand`. Use `-AndroidSdk` for a custom SDK path and `-Executable` for a separate compatible local executable.

The build runs without an attached phone. When you are ready, copy both files to your phone over USB in File Transfer mode. Open the APK on the phone to install it. In **New Vegas Recomp**, choose **Import game files (ZIP)** and select the ZIP. Keep both files private.

On a phone that already has this app, an in-place update requires the same signing key. If Android reports a signature mismatch, keep the existing installation and its saves while you resolve the signing key. When updating an existing installation with its game data already imported, the APK alone is enough.

## Manual build steps

## 2. Generate the native build input

```powershell
$game = 'C:\Games\Fallout New Vegas'
$local = 'C:\NewVegasLocal'
$ghidra = 'C:\Tools\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat'
New-Item -ItemType Directory -Force $local | Out-Null
$env:JAVA_HOME = 'C:\Tools\jdk-25'
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
python android/prepare.py $game --check-only
python android/prepare.py $game --ghidra $ghidra --output "$local\generated" --startup-group --virtual-tables --code-pointers --startup-roots android/startup_roots.txt
```

The generator needs a fresh output directory. If your compatible executable is stored separately, add `--exe "C:\path\to\FalloutNV.exe"` to both preparation commands and the ZIP command below.

## 3. Build and install your APK

```powershell
python tools/get_codecs.py
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21'
$env:Path = "$env:JAVA_HOME\bin;$env:Path"
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
gradle -p android assembleDebug "-PfnvGenerated=$local\generated" -PfnvNativeTls=true -PfnvFixedMemory=true
& "$env:ANDROID_HOME\platform-tools\adb.exe" install -r android/build/outputs/apk/debug/NewVegasRecompProbe-debug.apk
```

If your SDK or Java installation is elsewhere, use its actual path. With the phone unlocked, enable USB debugging and accept its prompt for your computer before running the install command. Android updates require the same signing key. Retain your local debug signing key when rebuilding.

## 4. Import your game files

```powershell
python -m tools.make_game_zip $game "$local\NewVegas-Private.zip"
& "$env:ANDROID_HOME\platform-tools\adb.exe" push "$local\NewVegas-Private.zip" /sdcard/Download/
```

Open **New Vegas Recomp**, select **Import game files (ZIP)** and pick the private ZIP. Wait for the import to finish, select your controls and tap **Play**. Installed official DLC in the ZIP is enabled when its complete file set is present. You can also import DLC later with **Import DLC (ZIP)** using a private ZIP with the add-on ESM and its matching BSA files under `Data/`.

## Setup APK only

With JDK 21, Gradle 8.9 and Android SDK platform 35, run `gradle -p setup assembleDebug`. This module builds without the PC executable, game assets, generated C, NDK or codecs. The result is `setup/build/outputs/apk/debug/NewVegasRecompSetup-debug.apk`.

Use [GitHub Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases) for the setup APK. The playable APK includes translated game code and should stay with its local builder. To enable the repository's optional local commit guard, run `git config core.hooksPath .githooks` in your checkout. On Unix, make the hook executable with `chmod +x .githooks/pre-commit`.
