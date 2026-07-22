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
    public volatile bool Enabled = true;

    public MediaWatcher(BleLink ble)
    {
        _ble = ble;
        _poll = new System.Threading.Timer(async _ => await Tick(), null,
                                           TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(3));
    }

    async Task Tick()
    {
        if (!Enabled || !_ble.IsUp) return;
        try
        {
            _mgr ??= await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            var s = _mgr.GetCurrentSession();
            if (s is null) { await SendClear(); return; }

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
            if (sig == _lastSig) return;
            _lastSig = sig;
            await _ble.Write(Protocol.SetMedia(playing, pos, dur, title));
        }
        catch { /* session vanished mid-query — next tick recovers */ }
    }

    async Task SendClear()
    {
        if (_lastSig.Length == 0) return;
        _lastSig = "";
        await _ble.Write(Protocol.SetMedia(false, 0, 0, ""));
    }

    public void Dispose() => _poll.Dispose();
}
