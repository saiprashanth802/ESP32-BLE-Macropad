namespace MacroPadDeck;

/// Streams now-playing (title + timeline) to the pad from a platform
/// IMediaSource, with Feishin Remote as a fallback/augment. Owns the polling
/// cadence, change-detection, heartbeat and the Feishin favorite merge — all
/// of which are identical across platforms; only the raw source differs.
///
/// Poll-based (1 s): far simpler than juggling session event storms, and the
/// pad extrapolates position between pushes anyway.
public sealed class MediaPusher : IDisposable, ICurrentTrack
{
    readonly IBleLink _ble;
    readonly IMediaSource _source;
    readonly FeishinSource? _feishin;
    readonly System.Threading.Timer _poll;

    string _lastSig = "";
    DateTime _lastPush = DateTime.MinValue;

    /// What the pad is currently displaying — the favorite guard checks this.
    volatile string _currentTitle = "";
    public string CurrentTitle => _currentTitle;
    public volatile bool Enabled = true;

    // The Feishin link is shared: a now-playing fallback here, and the control
    // channel for favoriting in DeckController. Owned by the front-end.
    public MediaPusher(IBleLink ble, IMediaSource source, FeishinSource? feishin)
    {
        _ble = ble;
        _source = source;
        _feishin = feishin;
        _poll = new System.Threading.Timer(async _ => await Tick(), null,
                                           TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(1));
    }

    static void Log(string m) => Diag.Log($"media: {m}");

    async Task Tick()
    {
        if (!Enabled) return;
        if (!_ble.IsUp) return;

        MediaPoll poll;
        try { poll = await _source.Poll(); }
        catch (Exception ex) { Log($"source EX {ex.GetType().Name} — skipped"); return; }

        switch (poll.State)
        {
            case MediaState.Unknown:
                return;                              // transient — leave the pad as-is

            case MediaState.Idle:
                if (await TryFeishin()) return;      // Feishin never reaches SMTC/MPRIS
                await SendClear();
                return;

            case MediaState.Track:
                await PushTrack(poll.Track);
                return;
        }
    }

    async Task PushTrack(TrackInfo t)
    {
        if (t.Title.Length == 0) { await SendClear(); return; }

        // Neither SMTC nor MPRIS models favorites, so borrow Feishin's flag when
        // it's clearly the same track (Feishin also feeds the OS session, so the
        // titles match).
        bool fav = _feishin is not null && _feishin.SongName.Length > 0 &&
                   t.Title.StartsWith(_feishin.SongName, StringComparison.OrdinalIgnoreCase) &&
                   _feishin.IsFavorite;

        // Push on any meaningful change; while playing, a ~2 s position bucket
        // refreshes the pad's extrapolation base periodically.
        string sig = $"{t.Title}|{t.Playing}|{t.DurSeconds}|{t.PosSeconds / 2}|{fav}";
        // Heartbeat: a paused track's signature never changes, and the pad drops
        // the strip after 30 s without a push — refresh before then.
        bool stale = (DateTime.UtcNow - _lastPush).TotalSeconds > 15;
        if (sig == _lastSig && !stale) return;

        _lastSig = sig;
        _lastPush = DateTime.UtcNow;
        _currentTitle = t.Title;
        bool ok = await _ble.Write(Protocol.SetMedia(t.Playing, t.PosSeconds, t.DurSeconds, t.Title, fav));
        Log($"push '{t.Title}' {t.PosSeconds}/{t.DurSeconds}s playing={t.Playing} fav={fav} src={t.SourceId} write={ok}");
    }

    /// Feishin Remote fallback. Shares the dedup/heartbeat logic so the pad sees
    /// one consistent stream regardless of which source produced it.
    async Task<bool> TryFeishin()
    {
        if (_feishin is null) return false;
        string title = _feishin.Title;
        // Stale guard: if the socket dropped, stop claiming the pad's strip.
        if (title.Length == 0 || (DateTime.UtcNow - _feishin.LastUpdate).TotalSeconds > 30)
            return false;
        int pos = _feishin.Position, dur = _feishin.Duration;
        bool playing = _feishin.Playing;
        bool fav = _feishin.IsFavorite;

        string sig = $"F|{title}|{playing}|{dur}|{pos / 2}|{fav}";
        bool stale = (DateTime.UtcNow - _lastPush).TotalSeconds > 15;
        if (sig == _lastSig && !stale) return true;

        _lastSig = sig;
        _lastPush = DateTime.UtcNow;
        _currentTitle = title;
        bool w = await _ble.Write(Protocol.SetMedia(playing, pos, dur, title, fav));
        Log($"feishin push '{title}' {pos}/{dur}s playing={playing} fav={fav} write={w}");
        return true;
    }

    async Task SendClear()
    {
        if (_lastSig.Length == 0) return;
        _lastSig = "";
        await _ble.Write(Protocol.SetMedia(false, 0, 0, ""));
    }

    public void Dispose() => _poll.Dispose();   // _feishin is owned by the front-end
}
