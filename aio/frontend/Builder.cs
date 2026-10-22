// KartPad-NX Builder: a one-window frontend for aio/pipeline/kpnx_aio.py.
//
// Built with the C# compiler that ships with Windows (.NET Framework 4.x, C# 5) so the exe
// itself needs nothing installed. It carries the pipeline scripts, the pinned tool manifest
// and nxshim.exe as resources; on Build it fetches the pinned embeddable Python into the data
// folder next to the exe, writes the scripts there and runs the pipeline, showing its "@@"
// JSON progress events and its log.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyTitle("KartPad-NX Builder")]
[assembly: AssemblyProduct("KartPad-NX Builder")]
[assembly: AssemblyVersion("1.0.0.0")]

namespace KartPadNX
{
    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

        [STAThread]
        static void Main()
        {
            try { SetProcessDPIAware(); } catch { }
            try { ServicePointManager.SecurityProtocol = (SecurityProtocolType)(3072 | 12288); } // TLS 1.2 | 1.3
            catch (NotSupportedException) { ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }

    sealed class MainForm : Form
    {
        [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint flags);
        const uint ES_CONTINUOUS = 0x80000000, ES_SYSTEM_REQUIRED = 0x00000001;

        [StructLayout(LayoutKind.Sequential)]
        struct OsVersionInfo { public int Size, Major, Minor, Build, Platform; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Csd; }
        [DllImport("ntdll.dll", CharSet = CharSet.Unicode)] static extern int RtlGetVersion(ref OsVersionInfo info);

        [StructLayout(LayoutKind.Sequential)]
        struct MemoryStatus { public uint Length, Load; public ulong TotalPhys, AvailPhys, TotalPage, AvailPage, TotalVirtual, AvailVirtual, AvailExtended; }
        [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);

        readonly string exeDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
        readonly JavaScriptSerializer json = new JavaScriptSerializer();
        TextBox discBox, outBox, dataBox, logBox;
        CheckBox rrBox;
        NumericUpDown jobsBox;
        Button buildButton, cancelButton, openButton;
        ProgressBar bar;
        Label stageLabel, detailLabel, toolkitLabel;
        Process proc;
        volatile bool cancelled;
        string outputPath;

        public MainForm()
        {
            Text = "KartPad-NX Builder";
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(760, 600);
            MinimumSize = new Size(640, 480);
            StartPosition = FormStartPosition.CenterScreen;

            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, Padding = new Padding(12) };
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            Controls.Add(layout);

            var intro = new Label
            {
                AutoSize = true, MaximumSize = new Size(720, 0), Margin = new Padding(0, 0, 0, 10),
                Text = "Builds the KartPad-NX launcher (mkw-launcher.nro) from your own Mario Kart Wii disc " +
                       "(PAL, RMCP01 rev 0). Everything it needs is downloaded into the work folder; nothing is installed."
            };
            layout.Controls.Add(intro, 0, 0);
            layout.SetColumnSpan(intro, 3);

            discBox = AddPathRow(layout, 1, "Disc image", "Choose your .iso / .wbfs / .rvz", BrowseDisc);
            outBox = AddPathRow(layout, 2, "SD card folder", null, delegate { BrowseFolder(outBox); });
            dataBox = AddPathRow(layout, 3, "Work folder", null, delegate { BrowseFolder(dataBox); });

            // Building Retro Rewind also puts its matching files (from Retro Rewind's full pack) in
            // the SD card folder, so the game and the files on the card are always the same version.
            rrBox = new CheckBox
            {
                Text = "Build Retro Rewind too (also downloads Retro Rewind's own files, about 2 GB the first time)",
                AutoSize = true, Checked = true
            };
            layout.Controls.Add(rrBox, 1, 4);
            layout.SetColumnSpan(rrBox, 2);

            var jobsLabel = new Label { Text = "Compile jobs", AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 6, 8, 0) };
            jobsBox = new NumericUpDown { Minimum = 0, Maximum = 64, Value = 0, Width = 60 };
            var jobsHint = new Label { Text = "0 = automatic (by CPU and RAM)", AutoSize = true, Margin = new Padding(8, 6, 0, 0) };
            var jobsPanel = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
            jobsPanel.Controls.Add(jobsBox);
            jobsPanel.Controls.Add(jobsHint);
            layout.Controls.Add(jobsLabel, 0, 6);
            layout.Controls.Add(jobsPanel, 1, 6);

            toolkitLabel = new Label { AutoSize = true, ForeColor = SystemColors.GrayText, Margin = new Padding(0, 6, 0, 6) };
            layout.Controls.Add(toolkitLabel, 1, 7);
            layout.SetColumnSpan(toolkitLabel, 2);

            var buttons = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, Margin = new Padding(0, 4, 0, 8) };
            buildButton = new Button { Text = "Build", Width = 110, Height = 30 };
            buildButton.Click += delegate { StartBuild(); };
            cancelButton = new Button { Text = "Cancel", Width = 90, Height = 30, Enabled = false };
            cancelButton.Click += delegate { CancelBuild(); };
            openButton = new Button { Text = "Open SD card folder", Width = 150, Height = 30, Enabled = false };
            openButton.Click += delegate { if (outputPath != null) Process.Start("explorer.exe", "\"" + outBox.Text.Trim() + "\""); };
            buttons.Controls.Add(buildButton);
            buttons.Controls.Add(cancelButton);
            buttons.Controls.Add(openButton);
            layout.Controls.Add(buttons, 1, 8);
            layout.SetColumnSpan(buttons, 2);

            stageLabel = new Label { AutoSize = true, Font = new Font(Font, FontStyle.Bold), Text = "Ready" };
            layout.Controls.Add(stageLabel, 0, 9);
            layout.SetColumnSpan(stageLabel, 3);
            bar = new ProgressBar { Dock = DockStyle.Fill, Height = 18, Margin = new Padding(0, 4, 0, 2) };
            layout.Controls.Add(bar, 0, 10);
            layout.SetColumnSpan(bar, 3);
            detailLabel = new Label { AutoSize = true, ForeColor = SystemColors.GrayText };
            layout.Controls.Add(detailLabel, 0, 11);
            layout.SetColumnSpan(detailLabel, 3);

            logBox = new TextBox
            {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false,
                Dock = DockStyle.Fill, Font = new Font("Consolas", 8.5f), BackColor = SystemColors.Window
            };
            layout.Controls.Add(logBox, 0, 12);
            layout.SetColumnSpan(logBox, 3);
            for (int i = 0; i < 12; i++) layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            LoadSettings();
            UpdateToolkitLabel();
            FormClosing += delegate(object s, FormClosingEventArgs e)
            {
                if (proc != null && !proc.HasExited)
                {
                    if (MessageBox.Show(this, "A build is running. Stop it and quit?", Text, MessageBoxButtons.YesNo) != DialogResult.Yes)
                    { e.Cancel = true; return; }
                    CancelBuild();
                }
                SaveSettings();
            };
        }

        TextBox AddPathRow(TableLayoutPanel layout, int row, string label, string cue, EventHandler browse)
        {
            layout.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 6, 8, 0) }, 0, row);
            var box = new TextBox { Dock = DockStyle.Fill, Margin = new Padding(0, 3, 6, 3) };
            layout.Controls.Add(box, 1, row);
            var b = new Button { Text = "Browse...", AutoSize = true };
            b.Click += browse;
            layout.Controls.Add(b, 2, row);
            return box;
        }

        void BrowseDisc(object sender, EventArgs e)
        {
            using (var d = new OpenFileDialog())
            {
                d.Title = "Your Mario Kart Wii disc image";
                d.Filter = "Wii disc images|*.iso;*.wbfs;*.rvz;*.wia;*.ciso;*.gcz;*.nfs|All files|*.*";
                if (d.ShowDialog(this) == DialogResult.OK) discBox.Text = d.FileName;
            }
        }

        void BrowseFolder(TextBox box)
        {
            using (var d = new FolderBrowserDialog())
            {
                d.SelectedPath = box.Text;
                if (d.ShowDialog(this) == DialogResult.OK) { box.Text = d.SelectedPath; UpdateToolkitLabel(); }
            }
        }

        // ---------------------------------------------------------------- settings ---

        string SettingsPath { get { return Path.Combine(exeDir, "KartPadNX-Builder.settings"); } }

        void LoadSettings()
        {
            outBox.Text = Path.Combine(exeDir, "SD card");
            dataBox.Text = DefaultWorkFolder();
            try
            {
                if (!File.Exists(SettingsPath)) return;
                foreach (string line in File.ReadAllLines(SettingsPath))
                {
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    string k = line.Substring(0, eq), v = line.Substring(eq + 1);
                    if (k == "disc") discBox.Text = v;
                    else if (k == "out") outBox.Text = v;
                    else if (k == "data") dataBox.Text = v;
                    else if (k == "rr") rrBox.Checked = v == "1";
                    else if (k == "jobs") { int j; if (int.TryParse(v, out j) && j >= 0 && j <= 64) jobsBox.Value = j; }
                }
            }
            catch { }
        }

        void SaveSettings()
        {
            try
            {
                File.WriteAllLines(SettingsPath, new[] {
                    "disc=" + discBox.Text, "out=" + outBox.Text, "data=" + dataBox.Text,
                    "rr=" + (rrBox.Checked ? "1" : "0"), "jobs=" + jobsBox.Value });
            }
            catch { }
        }

        // The build tools cannot handle every path: Ninja uses the ANSI code page, and the build
        // tree nests the work folder's path inside itself. Next to the exe when that is safe,
        // otherwise a short folder on the system drive.
        string DefaultWorkFolder()
        {
            string beside = Path.Combine(exeDir, "KartPadNX-Builder-data");
            if (WorkFolderProblem(beside) == null) return beside;
            string drive = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System));
            return Path.Combine(drive, "KartPadNX-data");
        }

        // Tested: a 95-character work folder builds both games (CMake hashes long object paths).
        const int MaxWorkFolderLength = 100;

        static string WorkFolderProblem(string path)
        {
            foreach (char c in path)
                if (c > 127) return "contains non-English characters, which the build tools cannot handle";
            if (path.StartsWith(@"\\")) return "is a network path";
            if (path.Length > MaxWorkFolderLength)
                return "is longer than " + MaxWorkFolderLength + " characters; the build tree nests it and would pass Windows' 260-character path limit";
            return null;
        }

        string FindToolkit()
        {
            var found = Directory.GetFiles(exeDir, "KartPadNX-Toolkit*.zip");
            Array.Sort(found);
            return found.Length > 0 ? found[found.Length - 1] : null;
        }

        // ------------------------------------------------------------------- bundle ---
        // The bundle zip (python/, tools/, toolkit/, bundle.json) is KartPadNX-Builder.data next
        // to the exe, or appended to the exe itself. The data file is preferred: Windows
        // Defender re-scans an executable's whole content on every launch (~10 s for this
        // payload), but not a data file. The pipeline reads the zip in place; here only Python
        // is unpacked, to run the pipeline.

        string ExePath { get { return Assembly.GetExecutingAssembly().Location; } }
        string bundlePath;
        long bundleStart = -2;
        Dictionary<string, object> bundleInfo;

        // Offset of the zip in bundlePath, or -1 when there is none: the end-of-central-directory
        // record gives the central directory's size and its offset from the start of the zip.
        long BundleStart()
        {
            if (bundleStart != -2) return bundleStart;
            bundleStart = -1;
            string data = Path.Combine(exeDir, "KartPadNX-Builder.data");
            bundlePath = File.Exists(data) ? data : ExePath;
            try
            {
                using (var f = File.OpenRead(bundlePath))
                {
                    int tail = (int)Math.Min(f.Length, 66 * 1024);
                    var buf = new byte[tail];
                    f.Seek(-tail, SeekOrigin.End);
                    f.Read(buf, 0, tail);
                    for (int i = tail - 22; i >= 0; i--)
                    {
                        if (buf[i] != 0x50 || buf[i + 1] != 0x4b || buf[i + 2] != 5 || buf[i + 3] != 6) continue;
                        long cdSize = BitConverter.ToUInt32(buf, i + 12), cdOffset = BitConverter.ToUInt32(buf, i + 16);
                        long start = f.Length - tail + i - cdSize - cdOffset;
                        if (start >= 0) bundleStart = start;
                        break;
                    }
                }
                if (bundleStart >= 0)
                    using (var z = OpenBundle())
                    {
                        var e = z.GetEntry("bundle.json");
                        if (e == null) { bundleStart = -1; return -1; }
                        using (var r = new StreamReader(e.Open())) bundleInfo = json.Deserialize<Dictionary<string, object>>(r.ReadToEnd());
                    }
            }
            catch { bundleStart = -1; }
            return bundleStart;
        }

        ZipArchive OpenBundle()
        {
            var f = File.OpenRead(bundlePath);
            return new ZipArchive(new SubStream(f, bundleStart), ZipArchiveMode.Read, false);
        }

        bool HasBundledToolkit() { return BundleStart() >= 0 && bundleInfo.ContainsKey("toolkit_sha256"); }

        // Python from the bundle (once per bundle), or its pinned download for an exe without one.
        string EnsurePython(string data)
        {
            if (BundleStart() < 0) return DownloadPython(data);
            string dir = Path.Combine(data, "tools", "python");
            string exe = Path.Combine(dir, "python.exe"), marker = Path.Combine(dir, ".bundle-id");
            string id = (string)bundleInfo["id"];
            if (File.Exists(exe) && File.Exists(marker) && File.ReadAllText(marker).Trim() == id) return exe;
            UI(delegate { stageLabel.Text = "Unpacking Python"; });
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            using (var z = OpenBundle())
                foreach (var e in z.Entries)
                {
                    if (!e.FullName.StartsWith("python/") || e.FullName.EndsWith("/")) continue;
                    string outPath = Path.Combine(dir, e.FullName.Substring(7).Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(outPath));
                    e.ExtractToFile(outPath, true);
                }
            File.WriteAllText(marker, id);
            return exe;
        }

        void UpdateToolkitLabel()
        {
            string tk = FindToolkit();
            if (tk != null) toolkitLabel.Text = "Toolkit: " + Path.GetFileName(tk) + " (next to the builder)";
            else if (HasBundledToolkit()) toolkitLabel.Text = "Toolkit and build tools: built in (only devkitPro is downloaded)";
            else
            {
                var m = Manifest();
                var spec = (Dictionary<string, object>)m["toolkit"];
                string url = spec["url"] as string;
                toolkitLabel.Text = string.IsNullOrEmpty(url)
                    ? "Toolkit: not found. Put KartPadNX-Toolkit.zip next to this program."
                    : "Toolkit: will be downloaded from " + new Uri(url).Host;
            }
        }

        // ------------------------------------------------------------------ resources ---

        static byte[] Resource(string name)
        {
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
            {
                if (s == null) throw new InvalidOperationException("missing resource " + name);
                var ms = new MemoryStream();
                s.CopyTo(ms);
                return ms.ToArray();
            }
        }

        Dictionary<string, object> Manifest()
        {
            return json.Deserialize<Dictionary<string, object>>(Encoding.UTF8.GetString(Resource("tools.json")));
        }

        static void WriteIfChanged(string path, byte[] data)
        {
            if (File.Exists(path))
            {
                var old = File.ReadAllBytes(path);
                if (old.Length == data.Length && Convert.ToBase64String(old) == Convert.ToBase64String(data)) return;
            }
            File.WriteAllBytes(path, data);
        }

        static string Sha256(string path)
        {
            using (var sha = SHA256.Create())
            using (var f = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant();
        }

        // ---------------------------------------------------------------------- build ---

        void StartBuild()
        {
            string disc = discBox.Text.Trim(), outDir = outBox.Text.Trim(), data = dataBox.Text.Trim();
            if (!File.Exists(disc)) { MessageBox.Show(this, "Choose your disc image first.", Text); return; }
            if (outDir.Length == 0 || data.Length == 0) { MessageBox.Show(this, "Choose the output and work folders.", Text); return; }
            try { data = Path.GetFullPath(data); dataBox.Text = data; } catch { }
            bool rr = rrBox.Checked;
            if (!CheckSystem(data, outDir, rr)) return;
            SaveSettings();
            cancelled = false;
            outputPath = null;
            logBox.Clear();
            SetRunning(true);
            int jobs = (int)jobsBox.Value;
            var t = new Thread(delegate() { BuildThread(disc, outDir, data, rr, jobs); });
            t.IsBackground = true;
            t.Start();
        }

        // Requirements of the downloaded tools (.NET 8, Python 3.14, LLVM: 64-bit Windows 10+)
        // and of the build itself (disk, memory, path). Hard limits stop; soft ones ask.
        bool CheckSystem(string data, string outDir, bool rr)
        {
            if (!Environment.Is64BitOperatingSystem)
            { MessageBox.Show(this, "KartPad-NX Builder needs 64-bit Windows 10 or 11.", Text); return false; }
            var v = new OsVersionInfo { Size = Marshal.SizeOf(typeof(OsVersionInfo)) };
            if (RtlGetVersion(ref v) == 0 && v.Major < 10)
            { MessageBox.Show(this, "KartPad-NX Builder needs Windows 10 or 11 (this is Windows " + v.Major + "." + v.Minor + ").", Text); return false; }

            string problem = WorkFolderProblem(data);
            if (problem != null)
            {
                string alt = Path.Combine(Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.System)), "KartPadNX-data");
                var r = MessageBox.Show(this, "The work folder path " + problem + ":\n\n" + data +
                    "\n\nUse " + alt + " instead?", Text, MessageBoxButtons.YesNoCancel);
                if (r == DialogResult.Cancel) return false;
                if (r == DialogResult.Yes) { dataBox.Text = data = alt; }
            }

            try
            {
                Directory.CreateDirectory(data);
                long free = new DriveInfo(Path.GetPathRoot(data)).AvailableFreeSpace;
                // The work folder keeps the extracted disc, the build and, for Retro Rewind, its
                // full pack (1.9 GB) in downloads.
                long need = (rr ? 12L : 9L) << 30;
                if (free < need && MessageBox.Show(this, string.Format(
                        "The work folder's drive has {0:N1} GB free; a build needs about {1} GB.\n\nTry anyway?",
                        free / (double)(1L << 30), need >> 30), Text, MessageBoxButtons.YesNo) != DialogResult.Yes) return false;
            }
            catch (Exception ex) { MessageBox.Show(this, "Cannot use the work folder: " + ex.Message, Text); return false; }

            // The SD card folder gets the game files (2.7 GB) and, for Retro Rewind, its files (2.2 GB).
            try
            {
                long free = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(outDir))).AvailableFreeSpace;
                long need = rr ? (5L << 30) : (3L << 30);
                if (free < need && MessageBox.Show(this, string.Format(
                        "The SD card folder's drive has {0:N1} GB free; its files need about {1} GB.\n\nTry anyway?",
                        free / (double)(1L << 30), need >> 30), Text, MessageBoxButtons.YesNo) != DialogResult.Yes) return false;
            }
            catch { }   // e.g. a network share: the pipeline reports a real problem

            var m = new MemoryStatus { Length = (uint)Marshal.SizeOf(typeof(MemoryStatus)) };
            if (GlobalMemoryStatusEx(ref m) && m.TotalPhys < (7UL << 30) && MessageBox.Show(this, string.Format(
                    "This PC has {0:N1} GB of RAM. The build compiles one file at a time below 8 GB and may still run out of memory.\n\nTry anyway?",
                    m.TotalPhys / (double)(1UL << 30)), Text, MessageBoxButtons.YesNo) != DialogResult.Yes) return false;
            return true;
        }

        void SetRunning(bool running)
        {
            buildButton.Enabled = !running;
            cancelButton.Enabled = running;
            discBox.Enabled = outBox.Enabled = dataBox.Enabled = rrBox.Enabled = jobsBox.Enabled = !running;
            if (running) openButton.Enabled = false;
            SetThreadExecutionState(running ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
        }

        void BuildThread(string disc, string outDir, string data, bool rr, int jobs)
        {
            int rc = -1;
            try
            {
                string app = Path.Combine(data, "app"), tools = Path.Combine(data, "tools");
                Directory.CreateDirectory(app);
                Directory.CreateDirectory(Path.Combine(tools, "bin"));
                WriteIfChanged(Path.Combine(app, "kpnx_aio.py"), Resource("kpnx_aio.py"));
                WriteIfChanged(Path.Combine(app, "nxtools.py"), Resource("nxtools.py"));
                WriteIfChanged(Path.Combine(app, "tools.json"), Resource("tools.json"));
                string shim = Path.Combine(tools, "bin", "nxshim.exe");
                WriteIfChanged(shim, Resource("nxshim.exe"));
                string python = EnsurePython(data);
                if (cancelled) return;

                var args = new StringBuilder();
                args.Append("-u ").Append(Q(Path.Combine(app, "kpnx_aio.py")));
                args.Append(" --data ").Append(Q(data)).Append(" --disc ").Append(Q(disc)).Append(" --out ").Append(Q(outDir));
                args.Append(" --exe-dir ").Append(Q(exeDir)).Append(" --shim ").Append(Q(shim));
                if (BundleStart() >= 0) args.Append(" --bundle ").Append(Q(bundlePath));
                if (rr) args.Append(" --rr");
                if (jobs > 0) args.Append(" --jobs ").Append(jobs);
                var psi = new ProcessStartInfo(python, args.ToString())
                {
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
                    WorkingDirectory = data
                };
                psi.EnvironmentVariables["PYTHONUTF8"] = "1";
                psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
                psi.EnvironmentVariables.Remove("PYTHONHOME");
                psi.EnvironmentVariables.Remove("PYTHONPATH");
                proc = Process.Start(psi);
                proc.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) Log(e.Data); };
                proc.BeginErrorReadLine();
                string line;
                while ((line = proc.StandardOutput.ReadLine()) != null) HandleLine(line);
                proc.WaitForExit();
                rc = proc.ExitCode;
            }
            catch (Exception ex)
            {
                Log("ERROR: " + ex.Message);
                UI(delegate { stageLabel.Text = "Failed"; detailLabel.Text = ex.Message; });
            }
            finally
            {
                bool ok = rc == 0 && !cancelled;
                UI(delegate
                {
                    SetRunning(false);
                    if (cancelled) { stageLabel.Text = "Cancelled"; detailLabel.Text = "Run Build again to continue where it stopped."; }
                    if (ok) { bar.Value = bar.Maximum; openButton.Enabled = true; }
                });
                proc = null;
            }
        }

        static string Q(string s)
        {
            if (s.Length > 0 && s.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return s;
            var sb = new StringBuilder("\"");
            int bs = 0;
            foreach (char c in s)
            {
                if (c == '\\') { bs++; continue; }
                sb.Append('\\', c == '"' ? bs * 2 + 1 : bs);
                bs = 0;
                sb.Append(c);
            }
            sb.Append('\\', bs * 2).Append('"');
            return sb.ToString();
        }

        string DownloadPython(string data)
        {
            var spec = (Dictionary<string, object>)Manifest()["python"];
            string dir = Path.Combine(data, "tools", "python");
            string exe = Path.Combine(dir, "python.exe");
            string marker = Path.Combine(dir, ".sha256");
            string want = (string)spec["sha256"];
            if (File.Exists(exe) && File.Exists(marker) && File.ReadAllText(marker).Trim() == want) return exe;

            UI(delegate { stageLabel.Text = "Getting Python"; });
            string zip = Path.Combine(data, "downloads", "python-embed.zip");
            Directory.CreateDirectory(Path.GetDirectoryName(zip));
            Log("python: " + spec["url"]);
            var done = new ManualResetEvent(false);
            Exception error = null;
            using (var wc = new WebClient())
            {
                wc.Headers[HttpRequestHeader.UserAgent] = "KartPadNX-Builder/1.0";
                wc.DownloadProgressChanged += delegate(object s, DownloadProgressChangedEventArgs e)
                { UI(delegate { SetProgress(e.BytesReceived, e.TotalBytesToReceive, "Python"); }); };
                wc.DownloadFileCompleted += delegate(object s, System.ComponentModel.AsyncCompletedEventArgs e) { error = e.Error; done.Set(); };
                wc.DownloadFileAsync(new Uri((string)spec["url"]), zip);
                done.WaitOne();
            }
            if (error != null) throw new Exception("Python download failed: " + error.Message);
            if (Sha256(zip) != want) { File.Delete(zip); throw new Exception("Python download is corrupt (SHA-256 mismatch)."); }
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            ZipFile.ExtractToDirectory(zip, dir);
            File.Delete(zip);
            File.WriteAllText(marker, want);
            return exe;
        }

        void CancelBuild()
        {
            cancelled = true;
            var p = proc;
            if (p == null) return;
            try
            {
                var k = Process.Start(new ProcessStartInfo("taskkill.exe", "/T /F /PID " + p.Id) { CreateNoWindow = true, UseShellExecute = false });
                k.WaitForExit(10000);
            }
            catch { }
        }

        // ------------------------------------------------------------------- events ---

        void HandleLine(string line)
        {
            if (!line.StartsWith("@@")) { Log(line); return; }
            Dictionary<string, object> ev;
            try { ev = json.Deserialize<Dictionary<string, object>>(line.Substring(2)); }
            catch { Log(line); return; }
            UI(delegate
            {
                object v;
                if (ev.TryGetValue("title", out v)) { stageLabel.Text = (string)v; detailLabel.Text = ""; bar.Style = ProgressBarStyle.Marquee; }
                if (ev.TryGetValue("progress", out v))
                {
                    long done = Convert.ToInt64(ev["done"]), total = Convert.ToInt64(ev["total"]);
                    string item = ev.ContainsKey("item") ? (string)ev["item"] : "";
                    string kind = (string)v;
                    if (kind == "compile") { SetProgress(done, total, item); detailLabel.Text = string.Format("{0}: step {1:N0} of {2:N0}", item, done, total); }
                    else if (kind == "download") SetProgress(done, total, item);
                    else { bar.Style = ProgressBarStyle.Marquee; detailLabel.Text = (kind == "extract" ? "Unpacking " : "Packing ") + item; }
                }
                if (ev.TryGetValue("error", out v))
                {
                    stageLabel.Text = "Build failed";
                    detailLabel.Text = "See the log below; the full log is in the work folder's logs directory.";
                    bar.Style = ProgressBarStyle.Continuous;
                    bar.Value = 0;
                }
                if (ev.TryGetValue("done", out v) && v is bool && (bool)v)
                {
                    outputPath = (string)ev["output"];
                    stageLabel.Text = "Done";
                    detailLabel.Text = "Copy everything in the SD card folder to the root of your SD card, then start MKW Launcher.";
                    bar.Style = ProgressBarStyle.Continuous;
                }
            });
        }

        void SetProgress(long done, long total, string item)
        {
            if (total <= 0) { bar.Style = ProgressBarStyle.Marquee; return; }
            bar.Style = ProgressBarStyle.Continuous;
            bar.Maximum = 1000;
            bar.Value = (int)Math.Max(0, Math.Min(1000, done * 1000 / total));
            if (item != null && done <= total && total > 1 << 20)
                detailLabel.Text = string.Format("{0}: {1:N0} of {2:N0} MB", item, done >> 20, total >> 20);
        }

        readonly StringBuilder pending = new StringBuilder();
        int flushQueued;

        void Log(string line)
        {
            lock (pending) pending.Append(line).Append("\r\n");
            if (Interlocked.Exchange(ref flushQueued, 1) == 0)
                UI(delegate
                {
                    Interlocked.Exchange(ref flushQueued, 0);
                    string text;
                    lock (pending) { text = pending.ToString(); pending.Clear(); }
                    if (logBox.TextLength > 400000) logBox.Text = logBox.Text.Substring(logBox.TextLength - 200000);
                    logBox.AppendText(text);
                });
        }

        void UI(MethodInvoker action)
        {
            if (IsDisposed) return;
            try { BeginInvoke(action); } catch (InvalidOperationException) { }
        }
    }

    // A read-only view of a stream from `start` to its end, so ZipArchive can read the zip
    // appended to the exe with offsets relative to the zip's own start.
    sealed class SubStream : Stream
    {
        readonly Stream inner;
        readonly long start;

        public SubStream(Stream inner, long start) { this.inner = inner; this.start = start; inner.Position = start; }
        public override bool CanRead { get { return true; } }
        public override bool CanSeek { get { return true; } }
        public override bool CanWrite { get { return false; } }
        public override long Length { get { return inner.Length - start; } }
        public override long Position { get { return inner.Position - start; } set { inner.Position = start + value; } }
        public override int Read(byte[] buffer, int offset, int count) { return inner.Read(buffer, offset, count); }
        public override long Seek(long offset, SeekOrigin origin)
        {
            long target = origin == SeekOrigin.Begin ? offset : origin == SeekOrigin.Current ? Position + offset : Length + offset;
            Position = target;
            return target;
        }
        public override void Flush() { }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        protected override void Dispose(bool disposing) { if (disposing) inner.Dispose(); base.Dispose(disposing); }
    }
}
