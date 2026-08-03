using System.Runtime.InteropServices;

namespace MacroPadDeck;

internal static class Win32
{
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern IntPtr SetWinEventHook(uint eventMin, uint eventMax, IntPtr mod,
        WinEventDelegate proc, uint idProcess, uint idThread, uint flags);
    [DllImport("user32.dll")] public static extern bool UnhookWinEvent(IntPtr hook);

    public delegate void WinEventDelegate(IntPtr hook, uint ev, IntPtr hwnd,
        int idObject, int idChild, uint thread, uint time);

    public const uint EVENT_SYSTEM_FOREGROUND = 0x0003;
    public const uint WINEVENT_OUTOFCONTEXT = 0x0000;

    public const int SW_RESTORE = 9, SW_MAXIMIZE = 3, SW_MINIMIZE = 6;
    public const uint SWP_NOZORDER = 0x0004, SWP_SHOWWINDOW = 0x0040;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    // ---- synthetic input ------------------------------------------------------
    // SendInput rather than the simpler keybd_event: keybd_event is superseded,
    // and SendInput's events cannot be interleaved with real ones mid-sequence,
    // which matters when we're firing Ctrl+C into whatever app happens to be
    // focused.

    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint n, INPUT[] inputs, int cbSize);

    /// Bumped by the system on every clipboard write. Polling this beats
    /// sleeping a fixed interval after Ctrl+C — slow apps (Electron, browsers)
    /// take far longer than a guess would allow, fast ones need no wait at all.
    [DllImport("user32.dll")] public static extern uint GetClipboardSequenceNumber();

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int count);

    /// "notepad.exe — Untitled" for a window handle, for diagnostics. Never
    /// throws; an unreadable window is reported rather than swallowed.
    public static string Describe(IntPtr hWnd)
    {
        if (hWnd == IntPtr.Zero) return "(no window)";
        string proc = "?";
        try
        {
            GetWindowThreadProcessId(hWnd, out uint pid);
            proc = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName;
        }
        catch { /* process may have exited */ }

        var sb = new System.Text.StringBuilder(256);
        GetWindowText(hWnd, sb, sb.Capacity);
        return $"{proc} — \"{sb}\"";
    }

    public const int INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const ushort VK_CONTROL = 0x11, VK_C = 0x43, VK_V = 0x56;

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT
    {
        public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT
    {
        public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL, wParamH; }

    /// The union must carry all three members even though only the keyboard one
    /// is used — SendInput validates cbSize against the full INPUT size (40
    /// bytes on x64), and a keyboard-only struct silently fails the call.
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion
    {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public HARDWAREINPUT hi;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public int type; public InputUnion U; }

    static INPUT Key(ushort vk, bool up) => new()
    {
        type = INPUT_KEYBOARD,
        U = new InputUnion { ki = new KEYBDINPUT { wVk = vk, dwFlags = up ? KEYEVENTF_KEYUP : 0 } },
    };

    /// Send Ctrl+<vk> as one atomic input batch.
    public static bool SendCtrl(ushort vk)
    {
        var seq = new[]
        {
            Key(VK_CONTROL, false), Key(vk, false),
            Key(vk, true),          Key(VK_CONTROL, true),
        };
        return SendInput((uint)seq.Length, seq, Marshal.SizeOf<INPUT>()) == seq.Length;
    }

    /// The dance Windows requires before a background process may steal focus:
    /// attach our input queue to the current foreground thread's, then ask.
    public static void ForceForeground(IntPtr hWnd)
    {
        if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
        IntPtr fg = GetForegroundWindow();
        uint fgThread = GetWindowThreadProcessId(fg, out _);
        uint us = GetCurrentThreadId();
        bool attached = fgThread != us && AttachThreadInput(us, fgThread, true);
        try { SetForegroundWindow(hWnd); }
        finally { if (attached) AttachThreadInput(us, fgThread, false); }
    }
}
