using System.Diagnostics;
using System.Text.RegularExpressions;

namespace MacroPadDeck;

/// Foreground-follow on GNOME Wayland, where X11ForegroundWatcher is blind:
/// the compositor never publishes native-Wayland windows through
/// _NET_ACTIVE_WINDOW, so the X11 path can read a window id but never its
/// pid/class. Instead this reads the focused window's wm_class from the
/// "Focused Window D-Bus" GNOME extension (focused-window-dbus@flexagoon.com),
/// which exports org.gnome.shell.extensions.FocusedWindow.Get() -> a JSON blob
/// on the session bus.
///
/// It polls via `gdbus` on a timer — the same model as the X11 watcher, in the
/// same shell-out style as the rest of the Linux integration (wl-copy, ydotool,
/// pactl). The extension also emits a FocusChanged signal; polling is used for
/// simplicity and because a ~0.7 s preset-follow latency is imperceptible.
///
/// Raises ExeChanged(wm_class); ProfileStore.ForExe matches that against each
/// profile's AppMatch list, so AppMatch entries are wm_class names here rather
/// than exe basenames. If the extension is absent/disabled, every call fails and
/// foreground-follow is quietly disabled (logged once).
public sealed class GnomeWaylandForegroundWatcher : IForegroundWatcher
{
    const string Dest  = "org.gnome.Shell";
    const string Path  = "/org/gnome/shell/extensions/FocusedWindow";
    const string Iface = "org.gnome.shell.extensions.FocusedWindow";

    // wm_class is a plain identifier in practice ("firefox", "org.kde.kicad",
    // "Code"), so pulling it out with a regex sidesteps GVariant/JSON unescaping
    // of the rest of the blob (titles can carry quotes and backslashes).
    static readonly Regex WmClassRx =
        new("\"wm_class\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"", RegexOptions.Compiled);

    readonly System.Threading.Timer _poll;
    string _last = "";
    bool _warned;

    public event Action<string>? ExeChanged;

    public GnomeWaylandForegroundWatcher()
    {
        // Probe once so a missing extension is a clear log line rather than a
        // silent no-follow.
        if (Query() is null)
            Diag.Log("gnome-fg: FocusedWindow D-Bus not answering — is the " +
                     "'Focused Window D-Bus' GNOME extension enabled? " +
                     "foreground-follow is off until it is.");
        else
            Diag.Log("gnome-fg: FocusedWindow D-Bus up — foreground-follow armed");

        _poll = new System.Threading.Timer(_ => Tick(), null,
            TimeSpan.FromMilliseconds(700), TimeSpan.FromMilliseconds(700));
    }

    void Tick()
    {
        string? wmClass = Query();
        if (string.IsNullOrEmpty(wmClass)) return;   // call failed, or desktop focused
        if (string.Equals(wmClass, _last, StringComparison.OrdinalIgnoreCase)) return;
        _last = wmClass;
        Diag.Log($"gnome-fg: focus -> {wmClass}");
        ExeChanged?.Invoke(wmClass);
    }

    /// The focused window's wm_class, "" when nothing/the desktop is focused,
    /// or null when the call itself failed (extension absent, shell busy).
    string? Query()
    {
        string? outp = GdbusGet();
        if (outp is null) return null;
        var m = WmClassRx.Match(outp);
        if (m.Success) return m.Groups[1].Value;
        // A successful call with no wm_class means nothing is focused ("{}") or
        // the focused surface has a null class — not a failure, just "no app".
        return outp.Contains("{}") || outp.Contains("\"wm_class\": null") ? "" : null;
    }

    string? GdbusGet()
    {
        try
        {
            var psi = new ProcessStartInfo("gdbus",
                $"call --session --dest {Dest} --object-path {Path} --method {Iface}.Get")
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            using var p = Process.Start(psi);
            if (p is null) return null;
            string outp = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(2000)) { try { p.Kill(); } catch { } return null; }
            if (p.ExitCode != 0)
            {
                if (!_warned) { Diag.Log("gnome-fg: gdbus Get failed (extension not enabled yet?)"); _warned = true; }
                return null;
            }
            _warned = false;
            return outp;
        }
        catch (Exception ex)
        {
            if (!_warned) { Diag.Log($"gnome-fg: gdbus unavailable: {ex.Message}"); _warned = true; }
            return null;
        }
    }

    public void Dispose() => _poll.Dispose();
}
