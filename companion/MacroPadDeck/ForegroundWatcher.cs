using System.Diagnostics;

namespace MacroPadDeck;

/// Raises ExeChanged("chrome") whenever a different process takes the
/// foreground. Must be created on a thread with a message pump (the UI
/// thread) — SetWinEventHook delivers via window messages.
public sealed class ForegroundWatcher : IForegroundWatcher
{
    readonly IntPtr _hook;
    readonly Win32.WinEventDelegate _proc;   // kept alive — the hook holds a raw pointer
    string _lastExe = "";

    public event Action<string>? ExeChanged;

    public ForegroundWatcher()
    {
        _proc = OnEvent;
        _hook = Win32.SetWinEventHook(Win32.EVENT_SYSTEM_FOREGROUND, Win32.EVENT_SYSTEM_FOREGROUND,
                                      IntPtr.Zero, _proc, 0, 0, Win32.WINEVENT_OUTOFCONTEXT);
    }

    void OnEvent(IntPtr hook, uint ev, IntPtr hwnd, int idObject, int idChild, uint thread, uint time)
    {
        try
        {
            Win32.GetWindowThreadProcessId(hwnd, out uint pid);
            if (pid == 0) return;
            string exe = Process.GetProcessById((int)pid).ProcessName;
            if (exe == _lastExe) return;
            _lastExe = exe;
            ExeChanged?.Invoke(exe);
        }
        catch { /* process died between event and query */ }
    }

    public void Dispose() { if (_hook != IntPtr.Zero) Win32.UnhookWinEvent(_hook); }
}
