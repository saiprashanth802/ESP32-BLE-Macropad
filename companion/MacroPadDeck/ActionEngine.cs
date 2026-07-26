using System.Diagnostics;
using System.IO;

namespace MacroPadDeck;

/// Executes a KeyBinding on Windows. Stateless; every method tolerates missing
/// targets (a bad path must never crash the tray app — worst case nothing
/// happens).
public sealed class ActionEngine : IActionEngine
{
    public void Execute(KeyBinding b)
    {
        try
        {
            switch (b.Type.ToLowerInvariant())
            {
                case "focusorlaunch": FocusOrLaunch(b); break;
                case "open":          Open(b);          break;
                case "run":           Run(b);           break;
                case "window":        Window(b.Target); break;
            }
        }
        catch (Exception ex) { Debug.WriteLine($"[action] {b.Type} '{b.Target}': {ex.Message}"); }
    }

    static void FocusOrLaunch(KeyBinding b)
    {
        string name = Path.GetFileNameWithoutExtension(b.Target);
        var proc = Process.GetProcessesByName(name)
                          .FirstOrDefault(p => p.MainWindowHandle != IntPtr.Zero);
        if (proc is not null)
        {
            Win32.ForceForeground(proc.MainWindowHandle);
            return;
        }
        Process.Start(new ProcessStartInfo(b.Target, b.Args) { UseShellExecute = true });
    }

    static void Open(KeyBinding b) =>
        Process.Start(new ProcessStartInfo(b.Target) { UseShellExecute = true });

    static void Run(KeyBinding b) =>
        Process.Start(new ProcessStartInfo(b.Target, b.Args)
        {
            UseShellExecute = false,
            CreateNoWindow = b.Hidden,
        });

    static void Window(string op)
    {
        IntPtr hwnd = Win32.GetForegroundWindow();
        if (hwnd == IntPtr.Zero) return;
        var screen = Screen.FromHandle(hwnd);
        var wa = screen.WorkingArea;

        switch (op.ToLowerInvariant())
        {
            case "maximize": Win32.ShowWindow(hwnd, Win32.SW_MAXIMIZE); break;
            case "minimize": Win32.ShowWindow(hwnd, Win32.SW_MINIMIZE); break;
            case "left":
                Win32.ShowWindow(hwnd, Win32.SW_RESTORE);
                Win32.SetWindowPos(hwnd, IntPtr.Zero, wa.Left, wa.Top, wa.Width / 2, wa.Height,
                                   Win32.SWP_NOZORDER | Win32.SWP_SHOWWINDOW);
                break;
            case "right":
                Win32.ShowWindow(hwnd, Win32.SW_RESTORE);
                Win32.SetWindowPos(hwnd, IntPtr.Zero, wa.Left + wa.Width / 2, wa.Top, wa.Width / 2, wa.Height,
                                   Win32.SWP_NOZORDER | Win32.SWP_SHOWWINDOW);
                break;
            case "nextmonitor":
            {
                var all = Screen.AllScreens;
                if (all.Length < 2) break;
                int cur = Array.FindIndex(all, s => s.DeviceName == screen.DeviceName);
                var next = all[(cur + 1) % all.Length].WorkingArea;
                Win32.ShowWindow(hwnd, Win32.SW_RESTORE);
                Win32.GetWindowRect(hwnd, out var r);
                int w = Math.Min(r.Right - r.Left, next.Width);
                int h = Math.Min(r.Bottom - r.Top, next.Height);
                Win32.SetWindowPos(hwnd, IntPtr.Zero, next.Left, next.Top, w, h,
                                   Win32.SWP_NOZORDER | Win32.SWP_SHOWWINDOW);
                break;
            }
        }
    }
}
