using System.Diagnostics;

namespace MacroPadDeck;

/// Clipboard-based selection capture and paste-back — the Linux counterpart of
/// WindowsTextCapture. Same contract, but the two hard parts have different
/// answers here.
///
/// **Synthesising the keystrokes.** Windows has SendInput. X11 has XTEST (what
/// xdotool drives). Wayland has neither: a client cannot inject input into
/// another client, by design. The portable way under both is ydotool, which
/// writes to a /dev/uinput virtual device — the compositor sees an ordinary
/// keyboard and cannot tell the difference. That costs a daemon and a group
/// membership (see docs/LINUX_REWRITE_SETUP.md), which is why it isn't the
/// default everywhere, but it is the only approach that works on GNOME Wayland.
///
/// **Aiming the paste at the right window.** Windows snapshots the foreground
/// HWND and refocuses it. Under X11 the same trick works via xdotool. Under
/// Wayland there is no way to either read or set the focused window — so this
/// takes the opposite approach and never lets focus move in the first place:
/// the preview is mapped non-focusable (see GtkRewritePreview), so the target
/// app keeps the keyboard the whole time and a blind Ctrl+V lands where the
/// Ctrl+C came from.
///
/// That leaves one hole — the user clicking the preview window anyway — so the
/// preview reports whether it ever took focus, and a paste is abandoned if it
/// did. Same principle as the Windows implementation: pasting a rewritten
/// paragraph into the wrong application is far worse than doing nothing.
public sealed class LinuxTextCapture : ITextCapture, IDisposable
{
    // evdev keycodes (linux/input-event-codes.h) — ydotool speaks these, not
    // X keysyms, because it is typing on a virtual keyboard rather than talking
    // to a display server.
    const int KeyLeftCtrl = 29, KeyC = 46, KeyV = 47;

    readonly bool _wayland;
    readonly Func<bool> _focusIsSafe;

    string? _target;             // X11 window id; unused (and unusable) on Wayland
    string? _savedClipboard;

    /// <param name="focusIsSafe">
    /// Answers "is the keyboard still where the capture came from?" — i.e. has
    /// our own preview window taken focus since Capture(). Under X11 the window
    /// snapshot makes this redundant, but it is the only guard available under
    /// Wayland.
    /// </param>
    public LinuxTextCapture(Func<bool> focusIsSafe)
    {
        _focusIsSafe = focusIsSafe;
        _wayland = string.Equals(Environment.GetEnvironmentVariable("XDG_SESSION_TYPE"),
                                 "wayland", StringComparison.OrdinalIgnoreCase);
        Log($"session = {(_wayland ? "wayland" : "x11")}, key injection = {(HasYdotool ? "ydotool" : "MISSING")}");
    }

    static void Log(string m) => Diag.Log($"capture: {m}");

    static bool HasYdotool => Which("ydotool") is not null;

    // ── ITextCapture ────────────────────────────────────────────────────────────

    public async Task<string?> Capture()
    {
        _target = _wayland ? null : ActiveWindow();
        if (!_wayland && _target is null) { Log("no active window"); return null; }

        _savedClipboard = GetClipboard();
        string before = _savedClipboard ?? "";

        if (!SendCtrl(KeyC)) { Log("could not synthesise Ctrl+C — is ydotoold running?"); return null; }

        // Wait for the app to actually answer the copy. Electron apps and
        // browser contenteditable (Gmail) are among the slower ones, hence the
        // generous ceiling — same reasoning as the Windows implementation.
        //
        // There is no clipboard sequence number here, so this compares content.
        // Re-copying an identical selection therefore looks like "nothing
        // happened" and falls through to the timeout; the value is still
        // returned below, so the flow proceeds correctly either way.
        string? text = null;
        for (int waited = 0; waited < 1200; waited += 40)
        {
            await Task.Delay(40);
            string? now = GetClipboard();
            if (now is null || now == before) continue;
            text = now;
            break;
        }

        text ??= GetClipboard();

        if (string.IsNullOrWhiteSpace(text))
        {
            Log("clipboard still empty after Ctrl+C — nothing selected");
            return null;
        }
        return text.Replace("\r\n", "\n");
    }

    public async Task<bool> Replace(string text)
    {
        if (string.IsNullOrEmpty(text)) return false;

        if (!SetClipboard(text)) { Log("could not set clipboard"); return false; }

        if (!await Refocus())
        {
            Log("target window is not focused — not pasting");
            await RestoreClipboard();
            _target = null;
            return false;
        }

        bool sent = SendCtrl(KeyV);
        if (!sent) Log("could not synthesise Ctrl+V");

        // Let the paste consume the clipboard before putting the old value back.
        await Task.Delay(250);
        await RestoreClipboard();
        _target = null;
        return sent;
    }

    public Task<bool> ToClipboard(string text)
    {
        // Deliberately drops the saved clipboard: the user asked for this value
        // to be there, so restoring over it later would be wrong.
        _savedClipboard = null;
        return Task.FromResult(SetClipboard(text));
    }

    public void Abandon()
    {
        _target = null;
        _ = RestoreClipboard();
    }

    Task RestoreClipboard()
    {
        if (_savedClipboard is null) return Task.CompletedTask;
        string s = _savedClipboard;
        _savedClipboard = null;
        SetClipboard(s);
        return Task.CompletedTask;
    }

    // ── focus ───────────────────────────────────────────────────────────────────

    /// X11: put the captured window back and confirm it got there, exactly as
    /// the Windows version does. Wayland: nothing to put back — assert instead
    /// that focus never left, and refuse the paste if it did.
    async Task<bool> Refocus()
    {
        if (_wayland)
        {
            if (_focusIsSafe()) return true;
            Log("preview window took focus — the target is no longer keyboard-focused");
            return false;
        }

        if (_target is null) { Log("no capture target"); return false; }
        Xdotool("windowactivate", "--sync", _target);

        // Focus changes are asynchronous even with --sync, so confirm rather
        // than assume before synthesising a paste.
        for (int waited = 0; waited < 600; waited += 50)
        {
            await Task.Delay(50);
            if (ActiveWindow() == _target) return true;
        }
        return false;
    }

    static string? ActiveWindow()
    {
        string? id = Xdotool("getactivewindow")?.Trim();
        return string.IsNullOrEmpty(id) ? null : id;
    }

    // ── key injection ───────────────────────────────────────────────────────────

    /// ydotool's `key` verb takes evdev `code:pressed` pairs. Press and release
    /// are spelled out rather than using the `ctrl+c` shorthand so the ordering
    /// is unambiguous: the modifier must be down before the letter and up after.
    static bool SendCtrl(int keycode)
    {
        var args = new[]
        {
            "key",
            $"{KeyLeftCtrl}:1",
            $"{keycode}:1",
            $"{keycode}:0",
            $"{KeyLeftCtrl}:0",
        };
        return RunOk("ydotool", args, YdotoolEnv());
    }

    /// ydotool reaches its daemon over a unix socket. The path differs between
    /// distro packagings (and between running ydotoold as a user service vs a
    /// system one), so honour an explicit YDOTOOL_SOCKET and otherwise try the
    /// XDG runtime location that a user-level ydotoold uses.
    static Dictionary<string, string> YdotoolEnv()
    {
        var env = new Dictionary<string, string>();
        if (Environment.GetEnvironmentVariable("YDOTOOL_SOCKET") is { Length: > 0 }) return env;

        string? runtime = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
        if (runtime is { Length: > 0 })
        {
            string sock = Path.Combine(runtime, ".ydotool_socket");
            if (File.Exists(sock)) env["YDOTOOL_SOCKET"] = sock;
        }
        return env;
    }

    // ── clipboard ───────────────────────────────────────────────────────────────

    /// wl-clipboard under Wayland, xclip under X11. Both are read through the
    /// same shape so the rest of the class doesn't care which is in play.
    ///
    /// `--no-newline` / `-o` are important: a trailing newline the source never
    /// had would be fed to the model and pasted back into the document.
    string? GetClipboard() => _wayland
        ? RunOut("wl-paste", new[] { "--no-newline", "--type", "text/plain" })
        : RunOut("xclip", new[] { "-selection", "clipboard", "-o" });

    bool SetClipboard(string s)
    {
        if (s.Length == 0) s = "";
        return _wayland
            ? RunIn("wl-copy", new[] { "--type", "text/plain" }, s)
            : RunIn("xclip", new[] { "-selection", "clipboard", "-in" }, s);
    }

    // ── process plumbing ────────────────────────────────────────────────────────

    static string? Which(string exe)
    {
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':'))
        {
            if (dir.Length == 0) continue;
            string p = Path.Combine(dir, exe);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    static ProcessStartInfo Psi(string exe, string[] args, IDictionary<string, string>? env = null)
    {
        var psi = new ProcessStartInfo(exe)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        if (env is not null) foreach (var (k, v) in env) psi.Environment[k] = v;
        return psi;
    }

    static string? RunOut(string exe, string[] args)
    {
        try
        {
            using var p = Process.Start(Psi(exe, args));
            if (p is null) return null;
            p.StandardInput.Close();
            string o = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(2000)) { try { p.Kill(true); } catch { } return null; }
            // wl-paste exits 1 on an empty clipboard, which is not an error here.
            return p.ExitCode == 0 ? o : null;
        }
        catch (Exception ex) { Log($"{exe}: {ex.Message}"); return null; }
    }

    static bool RunIn(string exe, string[] args, string stdin)
    {
        try
        {
            using var p = Process.Start(Psi(exe, args));
            if (p is null) return false;
            p.StandardInput.Write(stdin);
            p.StandardInput.Close();
            // wl-copy forks a server to own the selection and returns straight
            // away; xclip stays in the foreground until it has the selection.
            // Either way a couple of seconds is ample, and a timeout here must
            // not kill the process — that would drop the clipboard on the floor.
            p.WaitForExit(2000);
            return true;
        }
        catch (Exception ex) { Log($"{exe}: {ex.Message}"); return false; }
    }

    static bool RunOk(string exe, string[] args, IDictionary<string, string>? env = null)
    {
        try
        {
            using var p = Process.Start(Psi(exe, args, env));
            if (p is null) return false;
            p.StandardInput.Close();
            string err = p.StandardError.ReadToEnd();
            if (!p.WaitForExit(2000)) { try { p.Kill(true); } catch { } return false; }
            if (p.ExitCode != 0 && err.Length > 0) Log($"{exe}: {err.Trim()}");
            return p.ExitCode == 0;
        }
        catch (Exception ex) { Log($"{exe}: {ex.Message} — is it installed?"); return false; }
    }

    static string? Xdotool(params string[] args) => RunOut("xdotool", args);

    public void Dispose() { }
}
