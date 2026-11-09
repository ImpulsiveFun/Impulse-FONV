using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

internal sealed class ToolSetup
{
    internal const string SdkTerms = "https://developer.android.com/studio/terms";
    private const string SourceResource = "builder-source.zip";
    private readonly string cache;
    private readonly bool offline;
    private readonly Action<string> log;
    private readonly Action<string> reportStage;
    private readonly Func<string, bool> acceptLicense;

    internal ToolSetup(string cache, bool offline, Action<string> log, Action<string> reportStage,
        Func<string, bool> acceptLicense)
    {
        this.cache = cache;
        this.offline = offline;
        this.log = log;
        this.reportStage = reportStage;
        this.acceptLicense = acceptLicense;
    }

    internal string Prepare(string launcherDirectory, IDictionary<string, string> paths)
    {
        Directory.CreateDirectory(cache);
        string root = PrepareSource(launcherDirectory);
        Resolve(paths, "PythonCommand", "Python 3.12", "python-3.12.10", "python.exe",
            "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embeddable-amd64.zip",
            "156c7eea90d58cd7e91a23f28a0056616b13e9f4cf4901b7b99b837b7848c6da",
            // Allow the project scripts to import their sibling modules.
            staged => File.Delete(Path.Combine(staged, "python312._pth")));
        CheckGame(root, paths);
        Resolve(paths, "GradleCommand", "Gradle 8.9", "gradle-8.9", @"gradle-8.9\bin\gradle.bat",
            "https://services.gradle.org/distributions/gradle-8.9-bin.zip",
            "d725d707bfabd4dfdc958c624003b3c80accc03f7037b5122c4b1d0ef15cecab");
        ResolveJdk(paths, "GradleJavaHome", "21.0.13.0.2",
            "c5706a9cf482ef1a0622a86eea5b13d557b07f04554572af061e2d02b58c0f99");
        string generated = paths["GeneratedPath"];
        if (generated.Length == 0) generated = Path.Combine(paths["OutputPath"], "generated");
        if (!Directory.Exists(generated))
        {
            ResolveJdk(paths, "GhidraJavaHome", "25.0.5.0.2",
                "5da7525c8268a10a66bd76a39f4e9af24d4ebc974d42800a609852c498a68460");
            Resolve(paths, "GhidraHeadless", "Ghidra 12.2", "ghidra-12.2", @"ghidra_12.2_PUBLIC\support\analyzeHeadless.bat",
                "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.2_build/ghidra_12.2_PUBLIC_20261016.zip",
                "2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67");
        }
        if (!SdkReady(paths["AndroidSdk"])) paths["AndroidSdk"] = PrepareSdk(paths["GradleJavaHome"]);
        return root;
    }

    private void CheckGame(string root, IDictionary<string, string> paths)
    {
        reportStage("Checking your game files.");
        string args = "-B android/prepare.py --check-only " + BuildProcess.Quote(paths["GamePath"]);
        if (paths["Executable"].Length > 0) args += " --exe " + BuildProcess.Quote(paths["Executable"]);
        var info = new ProcessStartInfo(paths["PythonCommand"], args)
        {
            WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        using (var process = new Process { StartInfo = info })
        {
            DataReceivedEventHandler report = delegate(object sender, DataReceivedEventArgs e)
            {
                if (!String.IsNullOrWhiteSpace(e.Data)) log(e.Data);
            };
            process.OutputDataReceived += report;
            process.ErrorDataReceived += report;
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            if (process.ExitCode != 0)
                throw new InvalidDataException("The game files did not pass validation. See the build log.");
        }
    }

    internal string PrepareSource(string launcherDirectory)
    {
        using (var source = Assembly.GetExecutingAssembly().GetManifestResourceStream(SourceResource))
        {
            if (source == null)
            {
                if (File.Exists(Path.Combine(launcherDirectory, "Build-Windows.ps1"))) return launcherDirectory;
                throw new InvalidDataException("This EXE is missing its build source. Download the standalone release.");
            }
            string root = Path.Combine(cache, "source-" + Sha256(source).Substring(0, 16));
            source.Position = 0;
            if (!File.Exists(Path.Combine(root, "Build-Windows.ps1")))
            {
                reportStage("Unpacking the build source.");
                InstallArchive(root, "Build-Windows.ps1", stage => ExtractZip(source, stage));
            }
            return root;
        }
    }

    private void Resolve(IDictionary<string, string> paths, string key, string label, string folder,
        string entry, string url, string hash, Action<string> configure = null)
    {
        if (File.Exists(paths[key])) return;
        paths[key] = Package(label, folder, entry, url, hash, configure);
    }

    private void ResolveJdk(IDictionary<string, string> paths, string key, string version, string hash)
    {
        if (paths[key].Length != 0 && File.Exists(Path.Combine(paths[key], "bin", "javac.exe"))) return;
        string archive = "microsoft-jdk-" + version + "-windows-x64.zip";
        string compiler = Package("Java " + version.Split('.')[0], "jdk-" + version,
            Path.Combine("jdk-" + version + "+1", "bin", "javac.exe"),
            "https://aka.ms/download-jdk/" + archive, hash);
        paths[key] = Directory.GetParent(Path.GetDirectoryName(compiler)).FullName;
    }

    private string Package(string label, string folder, string entry, string url, string hash,
        Action<string> configure = null)
    {
        string target = Path.Combine(cache, folder);
        string installed = Path.Combine(target, entry);
        if (File.Exists(installed)) return installed;
        if (offline) throw new InvalidOperationException(label + " is missing. Turn off offline mode to download it.");
        reportStage("Downloading " + label + ".");
        string download = Path.Combine(cache, "download-" + Guid.NewGuid().ToString("N") + ".zip");
        try
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            using (var client = new WebClient())
            using (var source = client.OpenRead(url))
            using (var destination = File.Create(download))
            {
                var buffer = new byte[128 * 1024];
                const long interval = 64L * 1024 * 1024;
                long next = interval;
                int count;
                while ((count = source.Read(buffer, 0, buffer.Length)) != 0)
                {
                    destination.Write(buffer, 0, count);
                    if (destination.Length < next) continue;
                    log(label + " downloaded " + destination.Length / (1024 * 1024) + " MB.");
                    next += interval;
                }
            }
            reportStage("Checking the " + label + " download.");
            using (var file = File.OpenRead(download))
            {
                if (!String.Equals(Sha256(file), hash, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("The " + label + " download did not match its checksum.");
            }
            reportStage("Extracting " + label + ".");
            InstallArchive(target, entry, stage => ExtractWithWindowsTar(download, stage), configure);
        }
        catch (WebException error)
        {
            throw new IOException("Could not download " + label + ". " + error.Message, error);
        }
        finally { File.Delete(download); }
        return installed;
    }

    private static string Sha256(Stream stream)
    {
        using (var sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }

    private static void ExtractZip(Stream source, string stage)
    {
        using (var zip = new ZipArchive(source, ZipArchiveMode.Read, true))
            zip.ExtractToDirectory(stage);
    }

    private static void ExtractWithWindowsTar(string archive, string stage)
    {
        // Vendor archives can contain paths longer than .NET Framework's ZIP extractor accepts.
        Directory.CreateDirectory(stage);
        var info = new ProcessStartInfo
        {
            FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe"),
            Arguments = "-xf " + BuildProcess.Quote(archive) + " -C " + BuildProcess.Quote(stage),
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true
        };
        using (var process = Process.Start(info))
        {
            string error = process.StandardError.ReadToEnd();
            process.WaitForExit();
            if (process.ExitCode != 0) throw new IOException("Could not extract the tool archive. " + error);
        }
    }

    private static void InstallArchive(string target, string entry, Action<string> extract, Action<string> configure = null)
    {
        string stage = Path.Combine(Path.GetDirectoryName(target), ".x" + Guid.NewGuid().ToString("N").Substring(0, 8));
        try
        {
            extract(stage);
            if (!File.Exists(Path.Combine(stage, entry)))
                throw new InvalidDataException("The archive did not contain " + entry + ".");
            if (configure != null) configure(stage);
            try { Directory.Move(stage, target); }
            catch (IOException)
            {
                if (!File.Exists(Path.Combine(target, entry))) throw;
            }
        }
        finally
        {
            if (Directory.Exists(stage)) Directory.Delete(stage, true);
        }
    }

    internal static bool SdkReady(string sdk)
    {
        return !String.IsNullOrWhiteSpace(sdk) && new[]
        {
            @"platforms\android-35\android.jar", @"build-tools\34.0.0\aapt2.exe",
            @"ndk\27.2.12479018\source.properties", @"cmake\3.22.1\bin\cmake.exe"
        }.All(file => File.Exists(Path.Combine(sdk, file)));
    }

    private string PrepareSdk(string javaHome)
    {
        string sdk = Path.Combine(cache, "android-sdk");
        if (SdkReady(sdk)) return sdk;
        if (offline) throw new InvalidOperationException("The Android SDK is incomplete. Turn off offline mode to set it up.");
        using (File.Open(Path.Combine(cache, "android-sdk.lock"), FileMode.OpenOrCreate, FileAccess.Write, FileShare.None))
        {
            if (SdkReady(sdk)) return sdk;
            if (!acceptLicense(null)) throw new OperationCanceledException();
            string manager = Package("Android command-line tools", "android-cli-19",
                @"cmdline-tools\lib\sdkmanager-classpath.jar",
                "https://dl.google.com/android/repository/commandlinetools-win-13114758_latest.zip",
                "98b565cb657b012dae6794cefc0f66ae1efb4690c699b78a614b4a6a3505b003");
            Directory.CreateDirectory(sdk);
            reportStage("Installing the Android SDK. This may take a while.");
            var args = new[] { "-classpath", manager, "com.android.sdklib.tool.sdkmanager.SdkManagerCli",
                "--sdk_root=" + sdk, "--install", "platforms;android-35", "build-tools;34.0.0",
                "ndk;27.2.12479018", "cmake;3.22.1" };
            var info = new ProcessStartInfo
            {
                FileName = Path.Combine(javaHome, "bin", "java.exe"),
                Arguments = String.Join(" ", args.Select(BuildProcess.Quote)),
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            info.EnvironmentVariables["ANDROID_USER_HOME"] = Path.Combine(cache, "android-user");
            Directory.CreateDirectory(info.EnvironmentVariables["ANDROID_USER_HOME"]);
            using (var process = new Process { StartInfo = info })
            {
                process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e)
                {
                    if (!String.IsNullOrWhiteSpace(e.Data)) log(e.Data);
                };
                process.Start();
                process.BeginErrorReadLine();
                try
                {
                    ReadSdkOutput(process.StandardOutput, answer =>
                    {
                        process.StandardInput.WriteLine(answer ? "y" : "n");
                        process.StandardInput.Flush();
                    });
                    process.WaitForExit();
                    if (process.ExitCode != 0 || !SdkReady(sdk))
                        throw new IOException("The Android SDK setup did not finish. See the build log.");
                }
                finally { if (!process.HasExited) process.Kill(); }
            }
        }
        return sdk;
    }

    internal void ReadSdkOutput(TextReader output, Action<bool> respond)
    {
        var line = new StringBuilder();
        var license = new StringBuilder();
        bool readingLicense = false;
        int character;
        while ((character = output.Read()) != -1)
        {
            if (character == '\n' || character == '\r')
            {
                string content = line.ToString().Trim();
                line.Clear();
                if (content.Length == 0) continue;
                if (content.StartsWith("License ", StringComparison.OrdinalIgnoreCase)) readingLicense = true;
                if (readingLicense) license.AppendLine(content);
                else log(content);
                continue;
            }
            line.Append((char)character);
            if (character != ':' || !Regex.IsMatch(line.ToString(),
                @"^\s*Accept\?\s*(?:\(y/N\)|\[y/N\]):$", RegexOptions.IgnoreCase)) continue;
            if (license.Length == 0)
                throw new InvalidDataException("The Android SDK requested acceptance without displaying its license.");
            bool accepted = acceptLicense(license.ToString());
            respond(accepted);
            if (!accepted) throw new OperationCanceledException();
            readingLicense = false;
            license.Clear();
            line.Clear();
        }
        if (line.Length > 0) log(line.ToString());
    }
}
