using System.Diagnostics;

namespace MacroPadDeck;

/// Executes a KeyBinding on Linux/X11. Mirrors the Windows ActionEngine's four
/// verbs (focusOrLaunch / open / run / window) using the standard X11 CLIs:
/// wmctrl for activation and geometry, xdotool for minimise, xdg-open for
/// files/URLs. Every method tolerates missing targets or missing tools — a bad
/// binding must never crash the tray app.
public sealed class LinuxActionEngine : IActionEngine
{
    public void Execute(KeyBinding b)
    {
        try
        {
            switch (b.Type.ToLowerInvariant())
            {
                case "focusorlaunch": FocusOrLaunch(b); break;
                case "open":          Open(b.Target);   break;
                case "run":           Run(b);           break;
                case "window":        Window(b.Target); break;
            }
        }
        catch (Exception ex) { Diag.Log($"action: {b.Type} '{b.Target}': {ex.Message}"); }
    }

    /// Activate an existing window whose WM_CLASS matches the target; if there
    /// is none, launch it. `wmctrl -x -a` returns non-zero when no window matched.
    static void FocusOrLaunch(KeyBinding b)
    {
        if (Tool("wmctrl", "-x", "-a", b.Target) == 0) return;
        Launch(b.Target, b.Args);
    }

    static void Open(string target) => Tool("xdg-open", target);

    static void Run(KeyBinding b) => Launch(b.Target, b.Args);

    static void Window(string op)
    {
        switch (op.ToLowerInvariant())
        {
            case "maximize":
                Tool("wmctrl", "-r", ":ACTIVE:", "-b", "add,maximized_vert,maximized_horz");
                break;
            case "minimize":
            {
                // wmctrl can't iconify; xdotool can, but needs the window id first.
                string id = Capture("xdotool", "getactivewindow").Trim();
                if (id.Length > 0) Tool("xdotool", "windowminimize", id);
                break;
            }
            case "left":  Half(left: true);  break;
            case "right": Half(left: false); break;
            case "nextmonitor": NextMonitor(); break;
        }
    }

    /// Snap the active window to the left or right half of the primary screen.
    static void Half(bool left)
    {
        var (w, h) = DisplayGeometry();
        if (w == 0) return;
        int half = w / 2;
        int x = left ? 0 : half;
        // Un-maximise first, or wmctrl -e is ignored while the window is maximised.
        Tool("wmctrl", "-r", ":ACTIVE:", "-b", "remove,maximized_vert,maximized_horz");
        Tool("wmctrl", "-r", ":ACTIVE:", "-e", $"0,{x},0,{half},{h}");
    }

    /// Move the active window to the next connected monitor, preserving size.
    static void NextMonitor()
    {
        var monitors = Monitors();
        if (monitors.Count < 2) return;

        string id = Capture("xdotool", "getactivewindow").Trim();
        if (id.Length == 0) return;
        // getwindowgeometry --shell prints X=…,Y=…,WIDTH=…,HEIGHT=…
        string geom = Capture("xdotool", "getwindowgeometry", "--shell", id);
        int wx = ShellVar(geom, "X"), wy = ShellVar(geom, "Y");
        int ww = ShellVar(geom, "WIDTH"), wh = ShellVar(geom, "HEIGHT");

        // Find the monitor currently holding the window's top-left, jump to next.
        int cur = monitors.FindIndex(m => wx >= m.X && wx < m.X + m.W);
        if (cur < 0) cur = 0;
        var next = monitors[(cur + 1) % monitors.Count];
        int nx = next.X + Math.Max(0, (wx - monitors[cur].X));
        if (nx + ww > next.X + next.W) nx = next.X;
        Tool("wmctrl", "-r", ":ACTIVE:", "-b", "remove,maximized_vert,maximized_horz");
        Tool("wmctrl", "-r", ":ACTIVE:", "-e", $"0,{nx},{next.Y + wy},{Math.Min(ww, next.W)},{wh}");
    }

    record struct Mon(int X, int Y, int W, int H);

    /// Parse `xrandr --listmonitors` lines: " 0: +*HDMI-1 1920/… 0+0  HDMI-1".
    static List<Mon> Monitors()
    {
        var list = new List<Mon>();
        foreach (var line in Capture("xrandr", "--listmonitors").Split('\n'))
        {
            // token like "1920/597x1080/336+0+0"
            var tok = line.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                          .FirstOrDefault(t => t.Contains('x') && t.Contains('+'));
            if (tok is null) continue;
            try
            {
                var wh = tok.Split('x');
                int w = int.Parse(wh[0].Split('/')[0]);
                var rest = wh[1].Split('+');           // H/…, X, Y
                int h = int.Parse(rest[0].Split('/')[0]);
                int x = int.Parse(rest[1]), y = int.Parse(rest[2]);
                list.Add(new Mon(x, y, w, h));
            }
            catch { }
        }
        return list;
    }

    static (int w, int h) DisplayGeometry()
    {
        var s = Capture("xdotool", "getdisplaygeometry").Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return s.Length >= 2 && int.TryParse(s[0], out int w) && int.TryParse(s[1], out int h) ? (w, h) : (0, 0);
    }

    static int ShellVar(string blob, string name)
    {
        foreach (var l in blob.Split('\n'))
            if (l.StartsWith(name + "=", StringComparison.Ordinal) && int.TryParse(l[(name.Length + 1)..].Trim(), out int v))
                return v;
        return 0;
    }

    // ── process helpers ────────────────────────────────────────────────────────

    static void Launch(string target, string args)
    {
        try
        {
            Process.Start(new ProcessStartInfo(target, args ?? "") { UseShellExecute = false });
        }
        catch (Exception ex) { Diag.Log($"action: launch '{target}' failed: {ex.Message}"); }
    }

    /// Run a tool directly (args passed via ArgumentList, so no shell parsing or
    /// quoting is involved), wait briefly, return exit code (127 if not found).
    static int Tool(string file, params string[] args)
    {
        try
        {
            var psi = new ProcessStartInfo(file)
            { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var a in args) psi.ArgumentList.Add(a);
            var p = Process.Start(psi);
            if (p is null) return 127;
            // Drain stdout so a chatty tool can't fill the pipe and wedge us.
            p.StandardOutput.ReadToEnd();
            p.WaitForExit(4000);
            return p.HasExited ? p.ExitCode : 0;
        }
        catch { return 127; }
    }

    static string Capture(string file, params string[] args)
    {
        try
        {
            var psi = new ProcessStartInfo(file)
            { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var a in args) psi.ArgumentList.Add(a);
            var p = Process.Start(psi);
            if (p is null) return "";
            string outp = p.StandardOutput.ReadToEnd();
            p.WaitForExit(4000);
            return outp;
        }
        catch { return ""; }
    }
}
