using System.IO;
using Windows.Media.Control;

namespace MacroPadDeck;

/// Streams Windows' system-wide now-playing info (title + timeline) to the
/// pad. Covers every SMTC source — Feishin, Spotify, browsers — with one
/// API. Poll-based (3 s): far simpler than juggling the WinRT session event
/// storm, and the pad extrapolates the position between pushes anyway.
public sealed class MediaWatcher : IDisposable
{
    readonly BleLink _ble;
    readonly System.Threading.Timer _poll;
    GlobalSystemMediaTransportControlsSessionManager? _mgr;
    string _lastSig = "";
    int _emptyPolls;               // consecutive empty polls — drives recycling
    DateTime _lastPush = DateTime.MinValue;
    public volatile bool Enabled = true;

    public MediaWatcher(BleLink ble)
    {
        _ble = ble;
        _poll = new System.Threading.Timer(async _ => await Tick(), null,
                                           TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(3));
    }

    static void Log(string m)
    {
        try { File.AppendAllText(Path.Combine(ProfileStore.Dir, "deck.log"),
                                 $"{DateTime.Now:HH:mm:ss.fff} media: {m}\r\n"); } catch { }
    }

    async Task Tick()
    {
        if (!Enabled) { Log("disabled"); return; }
        if (!_ble.IsUp) { Log("link down"); return; }
        try
        {
            _mgr ??= await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            // GetCurrentSession() is null unless Windows has designated a
            // "current" app; fall back to any session that is actually playing,
            // then to the first one, so background players still register.
            var s = _mgr.GetCurrentSession();
            if (s is null)
            {
                var all = _mgr.GetSessions();
                s = all.FirstOrDefault(x => x.GetPlaybackInfo().PlaybackStatus ==
                        GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
                    ?? all.FirstOrDefault();
                if (s is null)
                {
                    // A manager reporting zero sessions is usually stale rather
                    // than genuinely idle; recycle it every few empty polls.
                    if (++_emptyPolls % 4 == 0) _mgr = null;
                    if (_lastSig.Length > 0 || _emptyPolls % 20 == 1)
                        Log($"no session (sessions={all.Count})");
                    await SendClear();
                    return;
                }
                _emptyPolls = 0;
                Log($"using fallback session: {s.SourceAppUserModelId}");
            }

            var props = await s.TryGetMediaPropertiesAsync();
            var tl = s.GetTimelineProperties();
            bool playing = s.GetPlaybackInfo().PlaybackStatus ==
                           GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;
            string title = props?.Title ?? "";
            if (title.Length == 0) { await SendClear(); return; }

            int pos = (int)tl.Position.TotalSeconds;
            int dur = (int)(tl.EndTime - tl.StartTime).TotalSeconds;

            // Push on any meaningful change; while playing, a 5 s position
            // bucket refreshes the pad's extrapolation base periodically.
            string sig = $"{title}|{playing}|{dur}|{pos / 5}";
            // Heartbeat: a paused track's signature never changes, and the pad
            // drops the strip after 30 s without a push — refresh before then.
            bool stale = (DateTime.UtcNow - _lastPush).TotalSeconds > 15;
            if (sig == _lastSig && !stale) return;
            _lastSig = sig;
            _lastPush = DateTime.UtcNow;
            bool ok = await _ble.Write(Protocol.SetMedia(playing, pos, dur, title));
            Log($"push '{title}' {pos}/{dur}s playing={playing} write={ok}");
        }
        catch (Exception ex)
        {
            // The SMTC manager and its session objects go stale whenever the
            // set of sessions changes (track end, app focus shift) — the proxy
            // then throws and reports zero sessions forever. Drop it so the
            // next tick requests a fresh one.
            _mgr = null;
            Log($"EX {ex.GetType().Name} 0x{ex.HResult:X8} — manager reset");
        }
    }

    async Task SendClear()
    {
        if (_lastSig.Length == 0) return;
        _lastSig = "";
        await _ble.Write(Protocol.SetMedia(false, 0, 0, ""));
    }

    public void Dispose() => _poll.Dispose();
}
