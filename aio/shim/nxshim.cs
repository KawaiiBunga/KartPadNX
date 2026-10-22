// nxshim: stands in for a devkitPro host program (aarch64-none-elf-g++, elf2nro, nacptool,
// bin2s, uam, aarch64-none-elf-pkg-config). Copies of this exe carry the real program's name;
// each forwards to `python nxtools.py <tool> <args>` with the console handles inherited, so
// Ninja and CMake see the tool's output and exit code. Python and nxtools.py are named by the
// KPNX_PYTHON / KPNX_NXTOOLS environment variables or by nxshim.cfg next to the exe.
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

static class NxShim
{
    static string Quote(string arg)
    {
        if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return arg;
        var sb = new StringBuilder("\"");
        int backslashes = 0;
        foreach (char c in arg)
        {
            if (c == '\\') { backslashes++; continue; }
            if (c == '"') sb.Append('\\', backslashes * 2 + 1);
            else sb.Append('\\', backslashes);
            backslashes = 0;
            sb.Append(c);
        }
        sb.Append('\\', backslashes * 2);
        sb.Append('"');
        return sb.ToString();
    }

    static string ToolFor(string exeName)
    {
        string n = exeName.ToLowerInvariant();
        if (n == "aarch64-none-elf-g++" || n == "aarch64-none-elf-gcc") return "link";
        if (n == "aarch64-none-elf-pkg-config" || n == "pkg-config") return "pkg-config";
        return n;
    }

    static int Main(string[] args)
    {
        string self = Process.GetCurrentProcess().MainModule.FileName;
        string tool = ToolFor(Path.GetFileNameWithoutExtension(self));
        string python = Environment.GetEnvironmentVariable("KPNX_PYTHON");
        string script = Environment.GetEnvironmentVariable("KPNX_NXTOOLS");
        string cfg = Path.Combine(Path.GetDirectoryName(self), "nxshim.cfg");
        if ((string.IsNullOrEmpty(python) || string.IsNullOrEmpty(script)) && File.Exists(cfg))
        {
            foreach (string line in File.ReadAllLines(cfg))
            {
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string key = line.Substring(0, eq).Trim(), value = line.Substring(eq + 1).Trim();
                if (key == "python" && string.IsNullOrEmpty(python)) python = value;
                else if (key == "nxtools" && string.IsNullOrEmpty(script)) script = value;
                else if (key.StartsWith("env.")) Environment.SetEnvironmentVariable(key.Substring(4), value);
            }
        }
        if (string.IsNullOrEmpty(python) || string.IsNullOrEmpty(script))
        {
            Console.Error.WriteLine("nxshim: KPNX_PYTHON/KPNX_NXTOOLS are not set and nxshim.cfg is missing");
            return 70;
        }
        var cmd = new StringBuilder();
        cmd.Append(Quote(script)).Append(' ').Append(Quote(tool));
        foreach (string a in args) cmd.Append(' ').Append(Quote(a));
        var psi = new ProcessStartInfo(python, cmd.ToString());
        psi.UseShellExecute = false;
        using (var p = Process.Start(psi))
        {
            p.WaitForExit();
            return p.ExitCode;
        }
    }
}
