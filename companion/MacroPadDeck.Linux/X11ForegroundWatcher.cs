using System.IO;
using System.Runtime.InteropServices;

namespace MacroPadDeck;

/// Raises ExeChanged("firefox") whenever a different window takes the
/// foreground, driving profile auto-follow — the X11 counterpart of the
/// Windows SetWinEventHook watcher. There is no foreground-change signal on
/// EWMH without an event loop, so this polls _NET_ACTIVE_WINDOW (cheap: two
/// property reads every 400 ms) and resolves the owning process via
/// _NET_WM_PID → /proc/<pid>/comm.
///
/// This is inherently X11-only; on Wayland there is no portable way to learn
/// the focused app, and LinuxProgram wires a no-op watcher instead.
public sealed class X11ForegroundWatcher : IForegroundWatcher
{
    readonly IntPtr _display;
    readonly IntPtr _root;
    readonly IntPtr _atomActive, _atomPid;
    readonly System.Threading.Timer? _poll;
    string _lastExe = "";

    public event Action<string>? ExeChanged;

    public bool Ok => _display != IntPtr.Zero;

    public X11ForegroundWatcher()
    {
        _display = XOpenDisplay(IntPtr.Zero);
        if (_display == IntPtr.Zero)
        {
            Diag.Log("x11: XOpenDisplay failed — foreground-follow disabled");
            return;
        }
        _root = XDefaultRootWindow(_display);
        _atomActive = XInternAtom(_display, "_NET_ACTIVE_WINDOW", false);
        _atomPid = XInternAtom(_display, "_NET_WM_PID", false);
        _poll = new System.Threading.Timer(_ => Tick(), null,
                                           TimeSpan.FromMilliseconds(500), TimeSpan.FromMilliseconds(400));
    }

    void Tick()
    {
        try
        {
            IntPtr win = GetWindowProp(_root, _atomActive);
            if (win == IntPtr.Zero) return;

            IntPtr pidVal = GetWindowProp(win, _atomPid);
            long pid = pidVal.ToInt64();
            if (pid <= 0) return;

            string exe = ProcName(pid);
            if (exe.Length == 0 || exe == _lastExe) return;
            _lastExe = exe;
            ExeChanged?.Invoke(exe);
        }
        catch { /* window vanished between reads, or X hiccup — try next tick */ }
    }

    /// Reads a single format-32 property value (a window id or a pid). X stores
    /// format-32 properties one-per-C-long, so each element is pointer-width on
    /// a 64-bit client and reads cleanly as an IntPtr.
    IntPtr GetWindowProp(IntPtr window, IntPtr atom)
    {
        int status = XGetWindowProperty(_display, window, atom,
            IntPtr.Zero, (IntPtr)1, false, AnyPropertyType,
            out _, out _, out IntPtr nitems, out _, out IntPtr prop);
        if (status != 0 || prop == IntPtr.Zero) return IntPtr.Zero;
        try { return nitems.ToInt64() >= 1 ? Marshal.ReadIntPtr(prop) : IntPtr.Zero; }
        finally { XFree(prop); }
    }

    static string ProcName(long pid)
    {
        try
        {
            // comm is the short name (≤15 chars) and matches how users name apps
            // in AppMatch; fall back to the exe basename if comm is unavailable.
            string comm = $"/proc/{pid}/comm";
            if (File.Exists(comm)) return File.ReadAllText(comm).Trim();
            string exe = $"/proc/{pid}/exe";
            return Path.GetFileName(new FileInfo(exe).LinkTarget ?? "");
        }
        catch { return ""; }
    }

    public void Dispose()
    {
        _poll?.Dispose();
        if (_display != IntPtr.Zero) XCloseDisplay(_display);
    }

    // ── libX11 ───────────────────────────────────────────────────────────────
    static readonly IntPtr AnyPropertyType = IntPtr.Zero;   // 0 = AnyPropertyType

    [DllImport("libX11.so.6")] static extern IntPtr XOpenDisplay(IntPtr display);
    [DllImport("libX11.so.6")] static extern int XCloseDisplay(IntPtr display);
    [DllImport("libX11.so.6")] static extern IntPtr XDefaultRootWindow(IntPtr display);
    [DllImport("libX11.so.6")] static extern IntPtr XInternAtom(IntPtr display, string name, bool onlyIfExists);
    [DllImport("libX11.so.6")] static extern int XFree(IntPtr data);

    [DllImport("libX11.so.6")]
    static extern int XGetWindowProperty(
        IntPtr display, IntPtr window, IntPtr property,
        IntPtr longOffset, IntPtr longLength, bool delete, IntPtr reqType,
        out IntPtr actualType, out int actualFormat,
        out IntPtr nItems, out IntPtr bytesAfter, out IntPtr prop);
}
