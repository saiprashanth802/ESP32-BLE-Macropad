using System.Runtime.InteropServices;
using System.Windows.Input;

namespace MacroPadDeck;

/// Low-level keyboard capture for the shortcut recorder.
///
/// Why a WH_KEYBOARD_LL hook and not WPF's PreviewKeyDown: WPF sees a key only
/// after the shell has had it, so Win+L locked the PC, Win+D showed the desktop
/// and PrintScreen fired while the user was trying to *record* them (reported
/// 2026-09-15). A low-level hook runs before any of that. Every event is
/// swallowed while capturing — modifier downs included, which is what stops
/// the OS from ever seeing a Win chord — and the hook stays up after the chord
/// until all modifiers are released, so a trailing Win-up can't open Start.
///
/// The hook callback runs on the installing thread, which must pump messages:
/// install from the UI thread only. Stop() on focus loss, always — a forgotten
/// hook swallows the whole keyboard.
sealed class ShortcutCapture : IDisposable
{
    const int WH_KEYBOARD_LL = 13;
    const int WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105;

    delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SetWindowsHookEx(int id, HookProc proc, IntPtr hMod, uint threadId);
    [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr hook);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr hook, int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string? name);

    [StructLayout(LayoutKind.Sequential)]
    struct KBDLLHOOKSTRUCT { public uint vkCode, scanCode, flags, time; public UIntPtr extra; }

    readonly HookProc _proc;          // kept alive: the OS holds only a raw pointer
    IntPtr _hook;
    bool _ctrl, _shift, _alt, _win;
    bool _recorded;

    /// (modifier bitmask Ctrl1 Shift2 Alt4 Win8, key) once a non-modifier key is pressed.
    public event Action<int, Key>? Chord;
    /// Bare Esc.
    public event Action? Cancelled;

    public bool Active => _hook != IntPtr.Zero;

    public ShortcutCapture() => _proc = Callback;

    public void Start()
    {
        if (Active) return;
        _ctrl = _shift = _alt = _win = _recorded = false;
        _hook = SetWindowsHookEx(WH_KEYBOARD_LL, _proc, GetModuleHandle(null), 0);
    }

    /// Called on focus loss. If a chord was just recorded and modifiers are still
    /// held, the hook stays up until they are released (see Callback) so the
    /// trailing modifier-ups are swallowed too; otherwise it comes down now.
    public void Stop()
    {
        if (!Active) return;
        if (_recorded && (_ctrl || _shift || _alt || _win)) return;
        Unhook();
    }

    /// The key was not bindable: forget it and keep listening.
    public void Reject() => _recorded = false;

    void Unhook()
    {
        if (!Active) return;
        UnhookWindowsHookEx(_hook);
        _hook = IntPtr.Zero;
    }

    IntPtr Callback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0) return CallNextHookEx(_hook, nCode, wParam, lParam);
        var k = Marshal.PtrToStructure<KBDLLHOOKSTRUCT>(lParam);
        int msg = (int)wParam;
        bool down = msg is WM_KEYDOWN or WM_SYSKEYDOWN;
        bool up   = msg is WM_KEYUP or WM_SYSKEYUP;
        if (!down && !up) return CallNextHookEx(_hook, nCode, wParam, lParam);

        switch (k.vkCode)
        {
            case 0xA2: case 0xA3: case 0x11: _ctrl  = down; break;   // L/R CONTROL, CONTROL
            case 0xA0: case 0xA1: case 0x10: _shift = down; break;   // L/R SHIFT, SHIFT
            case 0xA4: case 0xA5: case 0x12: _alt   = down; break;   // L/R MENU, MENU
            case 0x5B: case 0x5C:            _win   = down; break;   // L/R WIN
            default:
                if (down && !_recorded)
                {
                    var key = KeyInterop.KeyFromVirtualKey((int)k.vkCode);
                    int mod = (_ctrl ? 1 : 0) | (_shift ? 2 : 0) | (_alt ? 4 : 0) | (_win ? 8 : 0);
                    if (key == Key.Escape && mod == 0) { _recorded = true; Cancelled?.Invoke(); }
                    else { _recorded = true; Chord?.Invoke(mod, key); }
                }
                break;
        }

        // After the chord, keep eating events until the hands are off the modifiers,
        // then let go. Before the chord, eat everything.
        if (_recorded && !_ctrl && !_shift && !_alt && !_win)
            Unhook();
        return (IntPtr)1;   // swallowed
    }

    public void Dispose() => Unhook();
}
