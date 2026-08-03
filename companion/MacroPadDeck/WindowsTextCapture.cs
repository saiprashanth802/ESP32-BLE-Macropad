using System.Diagnostics;
using WinFormsClipboard = System.Windows.Forms.Clipboard;

namespace MacroPadDeck;

/// Clipboard-based selection capture and paste-back.
///
/// Two things make this fiddly rather than trivial:
///
/// 1. Clipboard calls must run on an STA thread with a message pump. Pad
///    events arrive on the thread pool, so everything marshals through a
///    hidden control whose handle is created on the UI thread. (A captured
///    SynchronizationContext is not reliable here — Program builds this before
///    Application.Run, and WindowsFormsSynchronizationContext may not be
///    installed yet.)
///
/// 2. The preview window steals focus between capture and paste, so the target
///    window is snapshotted and explicitly refocused. The paste is *abandoned*
///    rather than attempted if focus can't be restored — pasting a rewritten
///    paragraph into the wrong application is far worse than doing nothing.
public sealed class WindowsTextCapture : ITextCapture, IDisposable
{
    readonly Control _marshal;      // handle lives on the UI thread
    IntPtr _target = IntPtr.Zero;   // window the selection came from
    string? _savedClipboard;        // restored after a paste

    public WindowsTextCapture()
    {
        _marshal = new Control();
        _ = _marshal.Handle;        // force creation now, on the UI thread
    }

    static void Log(string m) => Diag.Log($"capture: {m}");

    T OnUi<T>(Func<T> f)
    {
        if (!_marshal.InvokeRequired) return f();
        return (T)_marshal.Invoke(f)!;
    }

    // Clipboard is a shared system resource — another app holding it open makes
    // these calls fail transiently, which is normal rather than exceptional.
    static string? TryGetText()
    {
        for (int i = 0; i < 5; i++)
        {
            try { return WinFormsClipboard.ContainsText() ? WinFormsClipboard.GetText() : null; }
            catch { Thread.Sleep(30); }
        }
        return null;
    }

    static bool TrySetText(string s)
    {
        for (int i = 0; i < 5; i++)
        {
            try
            {
                if (s.Length == 0) WinFormsClipboard.Clear();
                else WinFormsClipboard.SetText(s);
                return true;
            }
            catch { Thread.Sleep(30); }
        }
        return false;
    }

    public async Task<string?> Capture()
    {
        _target = Win32.GetForegroundWindow();
        if (_target == IntPtr.Zero) { Log("no foreground window"); return null; }
        Log($"target = {Win32.Describe(_target)}");

        uint before = OnUi(() =>
        {
            _savedClipboard = TryGetText();
            return Win32.GetClipboardSequenceNumber();
        });

        if (!Win32.SendCtrl(Win32.VK_C)) { Log("SendInput(Ctrl+C) failed"); return null; }

        // Wait for the app to actually answer the copy. Gmail's contenteditable
        // is among the slower ones, hence the generous ceiling.
        string? text = null;
        for (int waited = 0; waited < 1200; waited += 40)
        {
            await Task.Delay(40);
            if (OnUi(Win32.GetClipboardSequenceNumber) == before) continue;
            text = OnUi(TryGetText);
            break;
        }

        if (string.IsNullOrWhiteSpace(text))
        {
            Log($"clipboard seq {before} unchanged after Ctrl+C — no selection in {Win32.Describe(_target)}");
            return null;
        }
        return text.Replace("\r\n", "\n");
    }

    public async Task<bool> Replace(string text)
    {
        if (_target == IntPtr.Zero) { Log("no capture target"); return false; }
        if (string.IsNullOrEmpty(text)) return false;

        if (!OnUi(() => TrySetText(text))) { Log("could not set clipboard"); return false; }

        Win32.ForceForeground(_target);

        // Focus changes are asynchronous. Confirm we actually got there before
        // synthesising a paste — a Ctrl+V aimed at the wrong window is the one
        // failure mode worth being paranoid about.
        bool focused = false;
        for (int waited = 0; waited < 600; waited += 50)
        {
            await Task.Delay(50);
            if (Win32.GetForegroundWindow() == _target) { focused = true; break; }
        }
        if (!focused)
        {
            Log("target window did not regain focus — not pasting");
            await RestoreClipboard();
            _target = IntPtr.Zero;
            return false;
        }

        bool sent = Win32.SendCtrl(Win32.VK_V);
        if (!sent) Log("SendInput(Ctrl+V) failed");

        // Let the paste consume the clipboard before putting the old value back.
        await Task.Delay(250);
        await RestoreClipboard();
        _target = IntPtr.Zero;
        return sent;
    }

    public Task<bool> ToClipboard(string text)
    {
        // Deliberately drops the saved clipboard: the user asked for this value
        // to be there, so restoring over it later would be wrong.
        _savedClipboard = null;
        return Task.FromResult(OnUi(() => TrySetText(text)));
    }

    public void Abandon()
    {
        _target = IntPtr.Zero;
        _ = RestoreClipboard();
    }

    /// Best-effort: only the text flavour is preserved. A clipboard holding an
    /// image or files can't be round-tripped this way, and the alternative
    /// (enumerating and re-setting every format) misbehaves with delayed-render
    /// sources far more often than it helps.
    Task RestoreClipboard()
    {
        if (_savedClipboard is null) return Task.CompletedTask;
        string s = _savedClipboard;
        _savedClipboard = null;
        try { OnUi(() => TrySetText(s)); } catch (Exception ex) { Debug.WriteLine(ex.Message); }
        return Task.CompletedTask;
    }

    public void Dispose() => _marshal.Dispose();
}
