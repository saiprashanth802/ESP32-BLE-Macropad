namespace MacroPadDeck;

/// The companion's half of the face: decides what mood the pad should lean
/// toward and keeps its beat clock in time. The pad owns every animation —
/// this only ever sends two small parameter packets.
///
/// Sources, fused by confidence-weighted mean each second:
///   music tags  — Feishin genre/BPM/rating/favourite, via moods.json
///   audio       — loudness → arousal, brightness → a little valence
///   app context — the foreground exe's category, via moods.json
///   clock       — late hours pull arousal down and set the late flag
/// Music outranks context while it plays. With no source at all it releases
/// the face (weight 0), and the pad falls back to its own mood.
public sealed class MoodEngine : IDisposable
{
    public readonly record struct Signal(double V, double A, double Conf, string Src);

    readonly IBleLink _ble;
    readonly MoodStore _store;
    readonly MediaPusher _media;
    readonly FeishinSource? _feishin;
    readonly IAudioAnalyzer? _audio;
    readonly System.Threading.Timer _moodTimer, _beatTimer;

    volatile string _exe = "";
    public volatile bool Enabled = true;

    // What the pad was last told, for change detection
    int _sentV = int.MinValue, _sentA, _sentW, _sentFlags;
    DateTime _sentAt = DateTime.MinValue;
    double _beatSentBpm, _beatSentAnchor;   // anchor in Stopwatch ms, pad-aligned
    double _beatSentAt;

    /// Last fused result, for --mood-test and the log.
    public string LastSummary { get; private set; } = "";

    public MoodEngine(IBleLink ble, MoodStore store, MediaPusher media,
                      FeishinSource? feishin, IAudioAnalyzer? audio)
    {
        _ble = ble; _store = store; _media = media; _feishin = feishin; _audio = audio;
        _moodTimer = new System.Threading.Timer(async _ => await MoodTick(), null,
                                                TimeSpan.FromSeconds(3), TimeSpan.FromSeconds(1));
        // Beat corrections are cheap and need to be timely — 250 ms keeps a
        // drift from ever showing for more than a beat or two.
        _beatTimer = new System.Threading.Timer(async _ => await BeatTick(), null,
                                                TimeSpan.FromSeconds(3), TimeSpan.FromMilliseconds(250));
    }

    static void Log(string m) => Diag.Log($"mood: {m}");

    public void OnForegroundExe(string exe) => _exe = exe;

    /// Re-send everything on the next tick — the pad holds it in RAM only.
    public void Invalidate() { _sentV = int.MinValue; _beatSentBpm = 0; }

    // ── Mood ────────────────────────────────────────────────────────────

    public List<Signal> Gather()
    {
        var cfg = _store.Config;
        var s = new List<Signal>();
        bool music = _media.PlayingMusic && _media.Enabled;
        AudioFeatures au = _audio?.Read() ?? AudioFeatures.Silent;

        if (music)
        {
            // Tags only count when Feishin is what's playing — its cached song
            // would otherwise lend a paused track's genre to whatever Spotify plays.
            bool feishinIsIt = _feishin is not null && _feishin.Playing &&
                               _feishin.SongName.Length > 0 &&
                               _media.CurrentTitle.StartsWith(_feishin.SongName, StringComparison.OrdinalIgnoreCase);
            MoodPoint? g = feishinIsIt ? MatchGenre(cfg, _feishin!.Genres) : null;
            MoodPoint mp = g ?? cfg.UnknownMusic;
            double v = mp.V, a = mp.A;
            if (feishinIsIt)
            {
                if (_feishin!.Rating > 0) v += (_feishin.Rating - 3) * 8;     // loved tracks read happier
                if (_feishin.IsFavorite) v += 12;
                if (_feishin.Bpm > 0) a += Math.Clamp((_feishin.Bpm - 110) * 0.4, -25, 25);
            }
            s.Add(new(v, a, g is not null ? 0.8 : 0.35, g is not null ? "tags" : "music"));

            if (au.Active)
            {
                // Loud and driving → wired; quiet → mellow. Bright timbre nudges happy.
                double la = -60 + 150 * au.Loudness;
                double lv = (au.Brightness - 0.5) * 40;
                s.Add(new(mp.V + lv, la, 0.6, "audio"));
            }
        }
        else
        {
            if (_exe.Length > 0 && cfg.Apps.TryGetValue(_exe, out var ctxName) &&
                cfg.Contexts.TryGetValue(ctxName, out var ctx))
                s.Add(new(ctx.V, ctx.A, 0.5, "app:" + ctxName));
        }

        if (IsLate(cfg)) s.Add(new(0, -45, 0.35, "late"));
        return s;
    }

    static MoodPoint? MatchGenre(MoodConfig cfg, string[] tags)
    {
        MoodPoint? best = null; int bestLen = 0;
        foreach (var tag in tags)
            foreach (var (key, mp) in cfg.Genres)
                if (key.Length > bestLen && tag.Contains(key, StringComparison.OrdinalIgnoreCase))
                { best = mp; bestLen = key.Length; }
        return best;
    }

    public static bool IsLate(MoodConfig cfg)
    {
        int h = DateTime.Now.Hour;
        return cfg.LateFrom <= cfg.LateTo ? h >= cfg.LateFrom && h < cfg.LateTo
                                          : h >= cfg.LateFrom || h < cfg.LateTo;
    }

    /// `--mood-set v a`: hold one fixed point at full weight, for sweeping the
    /// pad's mood map by hand on hardware. Null = normal fusion.
    public (int v, int a)? Pin;

    public (int v, int a, int w, byte flags) Fuse(List<Signal> s)
    {
        var cfg = _store.Config;
        if (Pin is { } p) return (Math.Clamp(p.v, -100, 100), Math.Clamp(p.a, -100, 100), 100, 0);
        if (s.Count == 0) return (0, 0, 0, 0);
        double cs = s.Sum(x => x.Conf);
        int v = (int)Math.Round(s.Sum(x => x.V * x.Conf) / cs);
        int a = (int)Math.Round(s.Sum(x => x.A * x.Conf) / cs);
        bool music = s.Any(x => x.Src is "tags" or "music" or "audio");
        int w = music ? cfg.WeightMusic : cfg.WeightContext;
        if (s.All(x => x.Src == "late")) w = cfg.WeightContext / 2;   // the clock alone is a nudge
        byte flags = 0;
        if (s.Any(x => x.Src == "late")) flags |= Protocol.MoodLate;
        if (!music && _exe.Length > 0 && cfg.Apps.TryGetValue(_exe, out var c) &&
            cfg.Contexts.TryGetValue(c, out var cp) && cp.Focused) flags |= Protocol.MoodFocused;
        return (Math.Clamp(v, -100, 100), Math.Clamp(a, -100, 100), w, flags);
    }

    async Task MoodTick()
    {
        try
        {
            if (!_ble.IsUp) return;
            var cfg = _store.Config;
            var sig = Gather();
            var (v, a, w, flags) = Enabled && cfg.Enabled ? Fuse(sig) : (0, 0, 0, (byte)0);
            LastSummary = $"v={v} a={a} w={w} flags={flags} src={string.Join(",", sig.Select(x => x.Src))}";

            // Push on a real change, and refresh well inside the ttl so the pad
            // never drops a still-valid opinion.
            bool changed = _sentV == int.MinValue || Math.Abs(v - _sentV) >= 8 ||
                           Math.Abs(a - _sentA) >= 8 || w != _sentW || flags != _sentFlags;
            bool heartbeat = w > 0 && (DateTime.UtcNow - _sentAt).TotalSeconds > Math.Min(30, cfg.TtlSeconds / 3);
            if (!changed && !heartbeat) return;
            if (w == 0 && _sentW == 0 && _sentV != int.MinValue) return;   // already released

            bool ok = await _ble.Write(Protocol.SetMood(v, a, w, cfg.TtlSeconds, flags));
            if (changed) Log($"{LastSummary} write={ok}");
            _sentV = v; _sentA = a; _sentW = w; _sentFlags = flags; _sentAt = DateTime.UtcNow;
        }
        catch (Exception ex) { Log($"tick EX {ex.GetType().Name}: {ex.Message}"); }
    }

    // ── Beat clock ──────────────────────────────────────────────────────

    async Task BeatTick()
    {
        try
        {
            if (!_ble.IsUp || _audio is null || !Enabled || !_store.Config.Enabled) return;
            if (!_media.PlayingMusic || !_media.Enabled) { await ReleaseBeat(); return; }
            var au = _audio.Read();
            if (!au.Active || au.Confidence < 40 || au.Bpm <= 0) { await ReleaseBeat(); return; }

            var cfg = _store.Config;
            double now = BeatTracker.NowMs();
            double period = 60000 / au.Bpm;
            // Where the pad should put a beat, in our clock: when it is heard,
            // i.e. the loopback beat plus the output device's latency.
            double heard = au.LastBeatMs + cfg.OutputLatencyMs;

            // Compare against the grid we last sent, wrapped to ±half a period
            bool resend = _beatSentBpm <= 0 || Math.Abs(au.Bpm - _beatSentBpm) > 1.0 ||
                          now - _beatSentAt > 4000;                // keepalive: pad drops it at 8 s
            if (!resend)
            {
                double sentPeriod = 60000 / _beatSentBpm;
                double d = (heard - _beatSentAnchor) % sentPeriod;
                if (d > sentPeriod / 2) d -= sentPeriod;
                if (d < -sentPeriod / 2) d += sentPeriod;
                resend = Math.Abs(d) > 60;
            }
            if (!resend) return;

            // The pad computes anchor = receive time − since. It receives
            // ~BleLatencyMs after we send, so add that to "since" to land the
            // anchor on the beat. Wrapped into [0, period) — the field is unsigned.
            double since = (now - heard) + cfg.BleLatencyMs;
            since = ((since % period) + period) % period;
            bool ok = await _ble.Write(Protocol.SetBeat(au.Bpm, (int)Math.Round(since), au.Confidence));
            if (Math.Abs(au.Bpm - _beatSentBpm) > 1.0)
                Log($"beat {au.Bpm:F1} bpm conf={au.Confidence} write={ok}");
            _beatSentBpm = au.Bpm; _beatSentAnchor = heard; _beatSentAt = now;
        }
        catch (Exception ex) { Log($"beat EX {ex.GetType().Name}: {ex.Message}"); }
    }

    async Task ReleaseBeat()
    {
        if (_beatSentBpm <= 0) return;
        _beatSentBpm = 0;
        await _ble.Write(Protocol.SetBeat(0, 0, 0));
    }

    public void Dispose() { _moodTimer.Dispose(); _beatTimer.Dispose(); }
}
