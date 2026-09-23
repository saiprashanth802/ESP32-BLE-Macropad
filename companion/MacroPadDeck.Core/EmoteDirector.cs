namespace MacroPadDeck;

/// The companion's half of the emotes. The pad fires its own events (connect,
/// typing, track change, favourite...) from the table this pushes to it; this
/// fires the ones only the PC can see — a drop in the music, switching into a
/// game or an editor, the clock passing into late night — as HCMD_EMOTE.
///
/// Also the editor's way to the pad: play an emote, preview a dance level,
/// and push/persist the whole face.json.
public sealed class EmoteDirector : IDisposable
{
    readonly IBleLink _ble;
    readonly FaceStore _store;
    public FaceStore Store => _store;
    readonly MoodStore _moods;
    readonly MediaPusher _media;
    readonly IAudioAnalyzer? _audio;
    readonly System.Threading.Timer _tick;
    readonly Dictionary<string, DateTime> _last = new(StringComparer.OrdinalIgnoreCase);
    readonly Queue<float> _loud = new();        // 250 ms loudness samples, ~6 s
    string _ctx = "";
    bool _wasLate;

    /// Hello byte 6, or -1 before any hello / on firmware without it.
    public int PadCaps { get; private set; } = -1;
    public bool PadSupported => PadCaps >= 0 && (PadCaps & Protocol.FaceCapsV3) != 0;
    public event Action? PadChanged;

    public EmoteDirector(IBleLink ble, FaceStore store, MoodStore moods, MediaPusher media,
                         IAudioAnalyzer? audio)
    {
        _ble = ble; _store = store; _moods = moods; _media = media; _audio = audio;
        _ble.EventReceived += OnPadEvent;
        _ble.LinkChanged += up => { if (!up) { PadCaps = -1; PadChanged?.Invoke(); } };
        _store.Reloaded += () => _ = PushConfig(persist: false);
        _wasLate = MoodEngine.IsLate(_moods.Config);   // starting late isn't an event
        _tick = new System.Threading.Timer(_ => Tick(), null,
                                           TimeSpan.FromSeconds(3), TimeSpan.FromMilliseconds(250));
    }

    static void Log(string m) => Diag.Log($"face: {m}");

    void OnPadEvent(byte op, byte[] p)
    {
        if (op != Protocol.EvHello) return;
        PadCaps = p.Length >= 7 ? p[6] : -1;
        PadChanged?.Invoke();
        // RAM only on every connect: a pad that missed a save still wears
        // the current table, and NVS isn't worn by reconnects.
        if (PadSupported) _ = PushConfig(persist: false);
    }

    /// Foreground app → its moods.json context; entering a new one is an event.
    public void OnForegroundExe(string exe)
    {
        var cfg = _moods.Config;
        string ctx = cfg.Apps.TryGetValue(exe, out var c) ? c : "";
        if (ctx == _ctx) return;
        _ctx = ctx;
        if (ctx.Length > 0) Fire("app:" + ctx);
    }

    void Tick()
    {
        try
        {
            bool late = MoodEngine.IsLate(_moods.Config);
            if (late && !_wasLate) Fire("late");
            _wasLate = late;

            // Drop: loud now against the quietest stretch 2-6 s ago. Only on
            // music, so a notification ding or a game explosion never counts.
            var au = _audio?.Read() ?? AudioFeatures.Silent;
            if (!au.Active || !_media.PlayingMusic) { _loud.Clear(); return; }
            _loud.Enqueue(au.Loudness);
            while (_loud.Count > 24) _loud.Dequeue();
            if (_loud.Count < 24) return;
            var s = _loud.ToArray();
            float now = (s[^1] + s[^2]) / 2;
            float before = s.Take(16).Min();
            if (now - before >= _store.Config.DropJump && now > 0.5f)
            {
                if (Fire("drop")) Log($"drop {before:F2} → {now:F2}");
                _loud.Clear();                  // one drop, not one per tick
            }
        }
        catch (Exception ex) { Log($"tick EX {ex.GetType().Name}: {ex.Message}"); }
    }

    /// A host rule: chance + cooldown, then play. True if it played.
    public bool Fire(string trigger)
    {
        if (!_store.Config.Host.TryGetValue(trigger, out var r) || r.Chance <= 0) return false;
        var now = DateTime.UtcNow;
        if (_last.TryGetValue(trigger, out var t) && (now - t).TotalSeconds < r.CooldownS) return false;
        _last[trigger] = now;
        if (Random.Shared.Next(100) >= r.Chance) return false;
        _ = Play(r.Emote);
        return true;
    }

    public async Task<bool> Play(string emote, int intensity = 100)
    {
        int id = Protocol.EmoteId(emote);
        if (id == 0xFF) id = Protocol.EmoteId(new[] { "vibing", "wink", "joy", "happy" }[Random.Shared.Next(4)]);
        if (id < 0 || !_ble.IsUp || !PadSupported) return false;
        bool ok = await _ble.Write(Protocol.PlayEmote(id, intensity));
        Log($"emote {emote} write={ok}");
        return ok;
    }

    /// Editor slider drag: the dance changes live, nothing is saved.
    public async Task PreviewDance(int level, int flairBars)
    {
        if (_ble.IsUp && PadSupported) await _ble.Write(Protocol.SetDance(level, flairBars, false));
    }

    /// Whole face.json to the pad: trigger table, then the dance settings.
    public async Task<bool> PushConfig(bool persist)
    {
        if (!_ble.IsUp || !PadSupported) return false;
        var c = _store.Config;
        bool ok = true;
        foreach (var w in Protocol.SetEmoteMap(c.PadMap(), persist)) ok &= await _ble.Write(w);
        ok &= await _ble.Write(Protocol.SetDance(c.DanceLevel, c.FlairBars, persist));
        Log($"config pushed persist={persist} ok={ok}");
        return ok;
    }

    public void Dispose() => _tick.Dispose();
}
