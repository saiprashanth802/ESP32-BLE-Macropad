using System.Diagnostics;
using System.Globalization;
using System.Text.RegularExpressions;

namespace MacroPadDeck;

/// Master-volume reader over `pactl` — the Linux counterpart of Windows'
/// CoreAudioVolumeSource. Works on PulseAudio and on PipeWire through its
/// pulse shim, which is what every mainstream desktop ships today.
///
/// The shape is deliberately different from the Windows one. Core Audio is an
/// in-process COM call, so VolumePusher's 150 ms poll is nearly free there;
/// here every reading would be two `fork`+`exec`s, i.e. ~13 processes a second
/// for a value that changes a handful of times an hour. So this class inverts
/// it: a long-lived `pactl subscribe` watches for sink events and refreshes a
/// cached reading, and Read() just hands back the cache. Turning the puck still
/// feels immediate — the subscribe event lands within a few ms of the volume
/// actually changing, which is faster than polling would have been anyway.
///
/// A slow backstop re-read covers the cases the event stream misses: the
/// default sink being switched, and pactl (or the whole sound server) dying and
/// coming back.
public sealed class PulseVolumeSource : IVolumeSource, IDisposable
{
    const string Sink = "@DEFAULT_SINK@";

    readonly CancellationTokenSource _cts = new();
    readonly System.Threading.Timer _backstop;
    readonly object _lock = new();

    VolumeInfo _cached = VolumeInfo.Unknown;
    Process? _subscribe;

    public PulseVolumeSource()
    {
        Refresh();
        _ = Task.Run(() => WatchLoop(_cts.Token));
        _backstop = new System.Threading.Timer(_ => Refresh(), null,
                                               TimeSpan.FromSeconds(5), TimeSpan.FromSeconds(5));
    }

    static void Log(string m) => Diag.Log($"volume: {m}");

    /// Contract is "never throw" — VolumePusher treats an exception as a skipped
    /// tick, but Unknown is the honest answer and keeps the puck showing mode.
    public VolumeInfo Read()
    {
        lock (_lock) return _cached;
    }

    // ── event stream ────────────────────────────────────────────────────────────

    /// `pactl subscribe` prints a line per server event and never exits. Restart
    /// it if it dies: a PipeWire restart takes the whole pulse socket with it,
    /// and without this the volume would silently freeze at its last reading
    /// until the app was restarted.
    async Task WatchLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                using var p = Start("subscribe");
                if (p is null) { await Task.Delay(5000, ct); continue; }

                lock (_lock) _subscribe = p;

                while (!ct.IsCancellationRequested)
                {
                    string? line = await p.StandardOutput.ReadLineAsync(ct);
                    if (line is null) break;                       // pactl exited
                    // "Event 'change' on sink #52" — server/sink-default events
                    // matter too, since they mean the default endpoint moved.
                    if (line.Contains("on sink", StringComparison.Ordinal) ||
                        line.Contains("on server", StringComparison.Ordinal))
                        Refresh();
                }
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex) { Log($"subscribe: {ex.Message}"); }

            lock (_lock) _subscribe = null;
            if (!ct.IsCancellationRequested) await Task.Delay(2000, ct).ContinueWith(_ => { });
        }
    }

    // ── reading ─────────────────────────────────────────────────────────────────

    void Refresh()
    {
        VolumeInfo v = ReadNow();
        lock (_lock) _cached = v;
    }

    VolumeInfo ReadNow()
    {
        string? vol = Run("get-sink-volume", Sink);
        if (vol is null) return VolumeInfo.Unknown;

        int level = ParseLevel(vol);
        if (level < 0) return VolumeInfo.Unknown;

        // A failed mute read is not worth discarding a good level over.
        bool muted = Run("get-sink-mute", Sink) is { } m &&
                     m.Contains("yes", StringComparison.OrdinalIgnoreCase);

        return new VolumeInfo(level, muted);
    }

    /// pactl prints per-channel percentages:
    ///   "Volume: front-left: 32768 /  50% / -18.06 dB,   front-right: ..."
    /// Take the loudest channel, matching how the desktop's own slider reports
    /// an unbalanced sink. Levels above 100% (allowed by PulseAudio) are clamped
    /// rather than dropped — the puck's bar is 0-100.
    static int ParseLevel(string text)
    {
        int best = -1;
        foreach (Match m in Regex.Matches(text, @"(\d+)%"))
            if (int.TryParse(m.Groups[1].Value, NumberStyles.Integer,
                             CultureInfo.InvariantCulture, out int pct))
                best = Math.Max(best, pct);

        return best < 0 ? -1 : Math.Clamp(best, 0, 100);
    }

    // ── process plumbing ────────────────────────────────────────────────────────

    static ProcessStartInfo Psi(params string[] args)
    {
        var psi = new ProcessStartInfo("pactl")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        // pactl localises "yes"/"no" for get-sink-mute, which the parse above
        // depends on. Pin it rather than trying to match every translation.
        psi.Environment["LC_ALL"] = "C";
        return psi;
    }

    static Process? Start(params string[] args)
    {
        try { return Process.Start(Psi(args)); }
        catch (Exception ex) { Log($"pactl {args[0]}: {ex.Message}"); return null; }
    }

    static string? Run(params string[] args)
    {
        try
        {
            using var p = Process.Start(Psi(args));
            if (p is null) return null;
            string outp = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(2000)) { try { p.Kill(true); } catch { } return null; }
            return p.ExitCode == 0 ? outp : null;
        }
        catch (Exception ex) { Log($"pactl {args[0]}: {ex.Message}"); return null; }
    }

    public void Dispose()
    {
        _cts.Cancel();
        _backstop.Dispose();
        Process? p;
        lock (_lock) p = _subscribe;
        try { p?.Kill(true); } catch { }
        _cts.Dispose();
    }
}
