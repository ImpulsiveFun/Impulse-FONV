using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;

[assembly: AssemblyTitle("New Vegas Recomp Builder")]
[assembly: AssemblyDescription("Local Windows build launcher for New Vegas Recomp on Android")]
[assembly: AssemblyVersion("2.0.0.0")]

internal static class Builder
{
    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new BuilderWindow(AppDomain.CurrentDomain.BaseDirectory));
    }
}

internal static class BuildProcess
{
    internal static ProcessStartInfo Create(string root, IEnumerable<KeyValuePair<string, string>> paths,
        bool keepZip, bool offline, IDictionary<string, string> settings)
    {
        var args = new StringBuilder("-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ")
            .Append(Quote(Path.Combine(root, "Build-Windows.ps1")));
        foreach (var path in paths)
            if (path.Value.Length > 0) args.Append(" -").Append(path.Key).Append(' ').Append(Quote(path.Value));
        if (keepZip) args.Append(" -SkipGameZip");
        if (offline) args.Append(" -Offline");
        var info = new ProcessStartInfo
        {
            FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"),
            Arguments = args.ToString(), WorkingDirectory = root, UseShellExecute = false,
            CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (string key in new[] { "GRADLE_USER_HOME", "ANDROID_USER_HOME", "GRADLE_OPTS", "APPDATA" })
        {
            string value;
            if (settings.TryGetValue(key, out value) && value.Length > 0) info.EnvironmentVariables[key] = value;
        }
        return info;
    }

    internal static string Quote(string argument)
    {
        return "\"" + Regex.Replace(argument, @"(\\*)(""|$)", match =>
            new string('\\', match.Groups[1].Length * 2) + (match.Groups[2].Length > 0 ? "\\\"" : "")) + "\"";
    }
}

internal sealed class BuilderWindow : Form
{
    private readonly string root;
    private readonly string cache;
    private readonly string settingsFile;
    private readonly Dictionary<string, string> settings = new Dictionary<string, string>();
    private readonly Dictionary<string, TextBox> paths = new Dictionary<string, TextBox>();
    private readonly TabControl tabs = new TabControl();
    private readonly TabPage buildTab = new TabPage("Build");
    private readonly TabPage toolsTab = new TabPage("Tools");
    private readonly TextBox log = new TextBox();
    private readonly Label status = new Label();
    private readonly Button build = new Button();
    private readonly Button openOutput = new Button();
    private readonly CheckBox offline = new CheckBox();
    private readonly CheckBox keepZip = new CheckBox();
    private Process running;
    private bool preparing;

    internal BuilderWindow(string directory)
    {
        root = directory;
        cache = Environment.GetEnvironmentVariable("NEW_VEGAS_BUILDER_CACHE");
        if (String.IsNullOrWhiteSpace(cache))
            cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NewVegasBuilder");
        string portableSettings = Path.Combine(root, "windows-builder-local.ini");
        settingsFile = File.Exists(portableSettings) ? portableSettings : Path.Combine(cache, "windows-builder-local.ini");
        LoadSettings();
        Text = "New Vegas Recomp Builder";
        Font = new Font("Segoe UI", 10);
        ClientSize = new Size(800, 540);
        MinimumSize = new Size(690, 530);
        StartPosition = FormStartPosition.CenterScreen;
        var surface = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, Padding = new Padding(16) };
        surface.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        surface.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        surface.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        Controls.Add(surface);

        var heading = new Label
        {
            AutoSize = true, Text = "New Vegas Recomp Builder",
            Font = new Font("Segoe UI", 20, FontStyle.Bold), ForeColor = Color.FromArgb(111, 64, 29),
            Margin = new Padding(2, 0, 0, 12)
        };
        surface.Controls.Add(heading, 0, 0);
        tabs.Dock = DockStyle.Fill;
        tabs.TabPages.Add(buildTab);
        tabs.TabPages.Add(toolsTab);
        surface.Controls.Add(tabs, 0, 1);

        var inputs = Grid(buildTab);
        Note(inputs, "Choose your installed PC game and a private folder for the APK and game ZIP.");
        PathRow(inputs, "GamePath", "Game installation", DefaultGame(), null);
        PathRow(inputs, "OutputPath", "Output folder",
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "New Vegas Android"), null);
        Note(inputs, "The first build downloads missing tools. Later builds can reuse the downloads.");
        log.Multiline = true;
        log.ReadOnly = true;
        log.ScrollBars = ScrollBars.Vertical;
        log.WordWrap = true;
        log.Dock = DockStyle.Fill;
        log.MinimumSize = new Size(0, 120);
        log.AccessibleName = "Build output";
        AddWide(inputs, log, SizeType.Percent);

        var tools = Grid(toolsTab);
        Note(tools, "Optional settings for using your own tools or rebuilding offline.");
        PathRow(tools, "Executable", "Separate EXE (optional)", "", "Windows executable (*.exe)|*.exe");
        PathRow(tools, "PythonCommand", "Python", FindOnPath("python.exe"), "Python executable (*.exe)|*.exe");
        string gradle = FindOnPath("gradle.bat");
        if (gradle.Length > 0 && !File.Exists(Path.Combine(Path.GetDirectoryName(gradle), "..", "lib", "gradle-launcher-8.9.jar")))
            gradle = "";
        PathRow(tools, "GradleCommand", "Gradle", gradle, "Gradle launcher (*.bat)|*.bat");
        PathRow(tools, "GradleJavaHome", "JDK 21 for Gradle", FindJdk("21"), null);
        PathRow(tools, "AndroidSdk", "Android SDK", FirstExisting(
            Environment.GetEnvironmentVariable("ANDROID_HOME"),
            Environment.GetEnvironmentVariable("ANDROID_SDK_ROOT"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk")), null);
        PathRow(tools, "GhidraHeadless", "Ghidra launcher", FindOnPath("analyzeHeadless.bat"), "Ghidra launcher (*.bat)|*.bat");
        PathRow(tools, "GhidraJavaHome", "JDK 25 for Ghidra", FindJdk("25"), null);
        PathRow(tools, "GeneratedPath", "Reuse translation (optional)", "", null);
        offline.Text = "Use cached dependencies only";
        offline.AutoSize = true;
        offline.Checked = Setting("Offline", "false") == "true";
        AddWide(tools, offline);
        keepZip.Text = "Keep my existing game ZIP and build only the APK";
        keepZip.AutoSize = true;
        keepZip.Checked = Setting("SkipGameZip", "false") == "true";
        AddWide(tools, keepZip);

        var footer = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true, Padding = new Padding(0, 8, 0, 0) };
        build.Text = "Build APK and ZIP";
        build.AutoSize = true;
        build.Click += delegate { StartBuild(); };
        keepZip.CheckedChanged += delegate { build.Text = keepZip.Checked ? "Build APK" : "Build APK and ZIP"; };
        build.Text = keepZip.Checked ? "Build APK" : "Build APK and ZIP";
        openOutput.Text = "Open output folder";
        openOutput.AutoSize = true;
        openOutput.Click += delegate
        {
            string output = Value("OutputPath");
            if (Directory.Exists(output)) Process.Start("explorer.exe", BuildProcess.Quote(output));
            else status.Text = "The output folder will be created when you build.";
        };
        status.Text = "Ready to build on this PC.";
        status.AutoSize = true;
        status.Margin = new Padding(12, 9, 0, 0);
        footer.Controls.Add(build);
        footer.Controls.Add(openOutput);
        footer.Controls.Add(status);
        surface.Controls.Add(footer, 0, 2);
    }

    private static TableLayoutPanel Grid(TabPage tab)
    {
        var grid = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, Padding = new Padding(10), AutoScroll = true };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        tab.Controls.Add(grid);
        return grid;
    }

    private static void AddWide(TableLayoutPanel grid, Control control, SizeType size = SizeType.AutoSize)
    {
        int row = grid.RowCount++;
        grid.RowStyles.Add(new RowStyle(size, size == SizeType.Percent ? 100 : 0));
        grid.Controls.Add(control, 0, row);
        grid.SetColumnSpan(control, 3);
    }

    private static void Note(TableLayoutPanel grid, string text)
    {
        AddWide(grid, new Label { Text = text, AutoSize = true, MaximumSize = new Size(580, 0), Margin = new Padding(3, 4, 0, 10) });
    }

    private void PathRow(TableLayoutPanel grid, string name, string label, string suggested, string filter)
    {
        int row = grid.RowCount++;
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var input = new TextBox { Text = Setting(name, suggested), Dock = DockStyle.Fill, AccessibleName = label };
        paths.Add(name, input);
        var browse = new Button { Text = "Browse…", AutoSize = true, AccessibleName = "Browse " + label };
        browse.Click += delegate
        {
            if (filter == null)
            {
                using (var dialog = new FolderBrowserDialog { Description = label, ShowNewFolderButton = name == "OutputPath" })
                {
                    if (Directory.Exists(input.Text)) dialog.SelectedPath = input.Text;
                    if (dialog.ShowDialog(this) == DialogResult.OK) input.Text = dialog.SelectedPath;
                }
            }
            else
            {
                using (var dialog = new OpenFileDialog { Filter = filter, CheckFileExists = true, Title = label })
                {
                    if (File.Exists(input.Text)) dialog.FileName = input.Text;
                    if (dialog.ShowDialog(this) == DialogResult.OK) input.Text = dialog.FileName;
                }
            }
        };
        grid.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left }, 0, row);
        grid.Controls.Add(input, 1, row);
        grid.Controls.Add(browse, 2, row);
    }

    private string Value(string key) { return paths[key].Text.Trim(); }
    private string Setting(string key, string fallback)
    {
        string value;
        return settings.TryGetValue(key, out value) ? value : fallback;
    }

    private void LoadSettings()
    {
        try
        {
            if (!File.Exists(settingsFile)) return;
            foreach (string line in File.ReadAllLines(settingsFile))
            {
                int separator = line.IndexOf('=');
                if (separator > 0) settings[line.Substring(0, separator)] = line.Substring(separator + 1);
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private void SaveSettings()
    {
        foreach (string key in paths.Keys) settings[key] = Value(key);
        settings["SkipGameZip"] = keepZip.Checked ? "true" : "false";
        settings["Offline"] = offline.Checked ? "true" : "false";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(settingsFile));
            File.WriteAllLines(settingsFile, settings.Select(pair => pair.Key + "=" + pair.Value));
        }
        catch (IOException) { Append("Could not save the optional local settings."); }
        catch (UnauthorizedAccessException) { Append("Could not save the optional local settings."); }
    }

    private bool Require(string key, Func<string, bool> valid, string message, TabPage tab)
    {
        string path = Value(key);
        if (path.IndexOfAny(Path.GetInvalidPathChars()) < 0 && valid(path)) return true;
        status.Text = message;
        tabs.SelectedTab = tab;
        paths[key].Focus();
        return false;
    }

    private async void StartBuild()
    {
        if (running != null || preparing) return;
        if (!Require("GamePath", path => Path.IsPathRooted(path) && File.Exists(Path.Combine(path, "Data", "FalloutNV.esm")),
                "Choose the game folder containing Data and FalloutNV.esm.", buildTab) ||
            !Require("OutputPath", path => Path.IsPathRooted(path) && path.Length > 3, "Choose a private output folder.", buildTab) ||
            !Require("Executable", path => File.Exists(path.Length == 0 ? Path.Combine(Value("GamePath"), "FalloutNV.exe") : path),
                "FalloutNV.exe is missing. Check the game folder or select it in Tools.", toolsTab))
            return;

        string output = Value("OutputPath");
        string generated = Value("GeneratedPath");
        if (generated.Length > 0 && !Require("GeneratedPath", Directory.Exists, "Select the local translation folder.", toolsTab))
            return;
        if (!keepZip.Checked && File.Exists(Path.Combine(output, "NewVegas-Private.zip")))
        {
            status.Text = "Choose a new folder, or keep the existing ZIP in Tools.";
            tabs.SelectedTab = buildTab;
            return;
        }

        log.Clear();
        SaveSettings();
        var buildPaths = paths.Keys.ToDictionary(key => key, Value);
        bool buildOffline = offline.Checked;
        bool skipZip = keepZip.Checked;
        preparing = true;
        SetBusy(true);
        status.Text = "Preparing the build tools.";
        try
        {
            var setup = new ToolSetup(cache, buildOffline, LogLine,
                license => (bool)Invoke(new Func<string, bool>(ShowLicense), license));
            string source = await Task.Run(() => setup.Prepare(root, buildPaths));
            StartProcess(BuildProcess.Create(source, buildPaths, skipZip, buildOffline, settings));
        }
        catch (OperationCanceledException)
        {
            status.Text = "Build cancelled. Android SDK terms were declined.";
        }
        catch (Exception error)
        {
            status.Text = "Build stopped. See the output above.";
            Append(error.Message);
        }
        finally
        {
            preparing = false;
            if (running == null) SetBusy(false);
        }
    }

    private void StartProcess(ProcessStartInfo info)
    {
        var buildLock = File.Open(Path.Combine(info.WorkingDirectory, ".builder.lock"), FileMode.OpenOrCreate,
            FileAccess.Write, FileShare.None);
        var process = new Process
        {
            StartInfo = info
        };
        process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) { LogLine(e.Data); };
        process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) { LogLine(e.Data); };
        try { process.Start(); }
        catch
        {
            process.Dispose();
            buildLock.Dispose();
            throw;
        }
        running = process;
        status.Text = "Building locally. This can take a while.";
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        System.Threading.ThreadPool.QueueUserWorkItem(delegate
        {
            process.WaitForExit();
            int code = process.ExitCode;
            buildLock.Dispose();
            BeginInvoke(new Action(delegate
            {
                running = null;
                process.Dispose();
                SetBusy(false);
                status.Text = code == 0 ? "Build finished. Open the output folder." : "Build stopped. See the output above.";
            }));
        });
    }

    private void SetBusy(bool busy)
    {
        if (busy) tabs.SelectedTab = buildTab;
        build.Enabled = !busy;
        openOutput.Enabled = !busy;
        toolsTab.Enabled = !busy;
        foreach (Control input in buildTab.Controls[0].Controls)
            if (input != log) input.Enabled = !busy;
    }

    private bool ShowLicense(string license)
    {
        using (var dialog = new Form
        {
            Text = "Android SDK license", StartPosition = FormStartPosition.CenterParent,
            ClientSize = new Size(650, 430), MinimizeBox = false, MaximizeBox = false
        })
        {
            var panel = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(12), RowCount = 3, ColumnCount = 1 };
            panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            var terms = new LinkLabel { Text = "Read the Android SDK terms on Google's website", AutoSize = true };
            terms.LinkClicked += delegate { Process.Start(ToolSetup.SdkTerms); };
            panel.Controls.Add(terms, 0, 0);
            panel.Controls.Add(new TextBox
            {
                Text = license ?? "The build needs Google's Android SDK. Read the terms at the link above before accepting and continuing.",
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill
            }, 0, 1);
            var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft };
            var decline = new Button { Text = "Decline", DialogResult = DialogResult.Cancel, AutoSize = true };
            buttons.Controls.Add(decline);
            buttons.Controls.Add(new Button { Text = "Accept and continue", DialogResult = DialogResult.OK, AutoSize = true });
            panel.Controls.Add(buttons, 0, 2);
            dialog.Controls.Add(panel);
            dialog.CancelButton = decline;
            dialog.ActiveControl = decline;
            return dialog.ShowDialog(this) == DialogResult.OK;
        }
    }

    private void LogLine(string line)
    {
        if (line == null || line.StartsWith("Adding ", StringComparison.Ordinal)) return;
        BeginInvoke(new Action<string>(Append), line);
    }

    private void Append(string line) { log.AppendText(line + Environment.NewLine); }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if ((running != null || preparing) && e.CloseReason == CloseReason.UserClosing)
        {
            status.Text = "Let the current build finish before closing.";
            e.Cancel = true;
        }
        base.OnFormClosing(e);
    }

    private static string FindOnPath(string name)
    {
        foreach (string directory in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
        {
            try
            {
                if (directory.Length == 0) continue;
                string path = Path.Combine(directory.Trim('"'), name);
                if (File.Exists(path) && path.IndexOf("\\WindowsApps\\", StringComparison.OrdinalIgnoreCase) < 0) return path;
            }
            catch (ArgumentException) { }
        }
        return "";
    }

    private static string FirstExisting(params string[] choices)
    {
        return choices.FirstOrDefault(path => Directory.Exists(path)) ?? "";
    }

    private static string DefaultGame()
    {
        string programFiles = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        return FirstExisting(Path.Combine(programFiles, "Steam", "steamapps", "common", "Fallout New Vegas"),
            Path.Combine(programFiles, "GOG Galaxy", "Games", "Fallout New Vegas"),
            @"C:\GOG Games\Fallout New Vegas");
    }

    private static string FindJdk(string version)
    {
        var candidates = new List<string> { Environment.GetEnvironmentVariable("JAVA_HOME") };
        string programFiles = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        foreach (string vendor in new[] { "Java", "Eclipse Adoptium", "Microsoft" })
        {
            string folder = Path.Combine(programFiles, vendor);
            if (Directory.Exists(folder)) candidates.AddRange(Directory.GetDirectories(folder, "jdk-" + version + "*"));
        }
        foreach (string candidate in candidates)
        {
            if (!Directory.Exists(candidate)) continue;
            string release = Path.Combine(candidate, "release");
            if (File.Exists(release) && Regex.IsMatch(File.ReadAllText(release), "JAVA_VERSION=\"" + version + "(?:\\.|\")"))
                return candidate;
        }
        return "";
    }
}
