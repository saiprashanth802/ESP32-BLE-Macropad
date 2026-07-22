namespace MacroPadDeck;

/// Glue: pad events → actions; foreground changes → pad preset + labels.
///
/// Profile model ("both"): the foreground app picks the profile, but a manual
/// preset switch on the pad wins for StickySeconds before auto-follow resumes.
public sealed class DeckController : IDisposable
{
    readonly BleLink _ble;
    readonly ProfileStore _store;
    int _activePreset = -1;               // pad's preset as we last knew it
    DateTime _manualUntil = DateTime.MinValue;
    string _lastExe = "";

    public event Action<string>? StatusChanged;   // tray tooltip / balloon text
    public void RaiseStatus(string s) => StatusChanged?.Invoke(s);

    public DeckController(BleLink ble, ProfileStore store)
    {
        _ble = ble; _store = store;
        _ble.EventReceived += OnPadEvent;
        _ble.LinkChanged += up =>
        {
            StatusChanged?.Invoke(up ? "Connected" : "Disconnected — retrying");
            // On (re)connect the hello event re-syncs _activePreset and labels.
        };
        _store.Reloaded += () =>
        {
            StatusChanged?.Invoke("Profiles reloaded");
            _ = Push(async () =>
            {
                await PushColors();
                if (_activePreset >= 0) await PushLabels(_activePreset);
            });
        };
    }

    public int ActivePreset => _activePreset;

    void OnPadEvent(byte op, byte[] p)
    {
        switch (op)
        {
            case Protocol.EvHello when p.Length >= 4:
                _activePreset = p[3];
                StatusChanged?.Invoke($"Pad online (fw v{p[0]}, preset {p[3]})");
                _ = Push(async () =>
                {
                    // Colors, eye color and the full key layout are re-pushed on
                    // every connect (RAM only, no NVS wear) — a pad that missed a
                    // commit still wakes up wearing the app's configuration.
                    await PushColors();
                    await PushEyes(_store.Config.EyeColor, persist: false);
                    await PushKeys(_store.Config, commit: false);
                    await PushLabels(_activePreset);
                });
                break;

            case Protocol.EvKey when p.Length >= 2:
            {
                var binding = _store.ForPreset(p[0])?.Keys.ElementAtOrDefault(p[1]);
                if (binding is not null)
                    ThreadPool.QueueUserWorkItem(_ => ActionEngine.Execute(binding));
                break;
            }

            case Protocol.EvPreset when p.Length >= 1:
                if (p[0] != _activePreset)
                {
                    // The user (or our own echo) switched — user switches stick.
                    _activePreset = p[0];
                    _manualUntil = DateTime.UtcNow.AddSeconds(_store.Config.StickySeconds);
                    _ = PushLabels(_activePreset);
                }
                break;
        }
    }

    /// Called by the ForegroundWatcher (UI thread).
    public void OnForegroundExe(string exe)
    {
        _lastExe = exe;
        if (!_ble.IsUp || DateTime.UtcNow < _manualUntil) return;

        var prof = _store.ForExe(exe);
        int target = prof?.Preset ?? _store.Config.DefaultPreset;
        if (target < 0 || target == _activePreset) return;

        _activePreset = target;   // optimistic; the pad echoes EvPreset back
        _ = Push(async () =>
        {
            await _ble.Write(Protocol.SetPreset(target));
            await PushLabels(target);
            await _ble.Write(Protocol.SetStatus(prof?.Name ?? exe));
        });
    }

    async Task PushLabels(int preset)
    {
        var prof = _store.ForPreset(preset);
        if (prof is null) return;
        for (int k = 0; k < prof.Keys.Count && k < 12; k++)
        {
            var b = prof.Keys[k];
            if (b.Type != "none" && b.Label.Length > 0)
                await _ble.Write(Protocol.SetLabel(preset, k, b.Label));
        }
    }

    /// Colors are per-preset device state (picker screen, eyes), so push all.
    async Task PushColors()
    {
        foreach (var prof in _store.Config.Profiles)
        {
            var cmd = Protocol.SetColor(prof.Preset, prof.Color);
            if (cmd is not null) await _ble.Write(cmd);
        }
    }

    /// Editor save: rewrite the app-managed keys, persist, sync eye color.
    public Task ApplyPadConfig(DeckConfig cfg) => Task.Run(async () =>
    {
        if (!_ble.IsUp) { StatusChanged?.Invoke("Pad offline — will sync on next connect"); return; }
        await PushKeys(cfg, commit: true);
        await PushEyes(cfg.EyeColor, persist: true);
        await PushColors();
        StatusChanged?.Invoke("Layout written to pad");
    });

    /// Keys typed "none" are left alone — firmware defaults stay untouched.
    async Task PushKeys(DeckConfig cfg, bool commit)
    {
        bool any = false;
        foreach (var prof in cfg.Profiles)
        {
            for (int k = 0; k < prof.Keys.Count && k < 12; k++)
            {
                var b = prof.Keys[k];
                byte[]? cmd = b.Type.ToLowerInvariant() switch
                {
                    "focusorlaunch" or "open" or "run" or "window" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaHost, 0, 0, 0, b.Label),
                    "shortcut" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaKey, (byte)b.Mod,
                            Protocol.HidKeys.FirstOrDefault(h => h.Name == b.Key).Hid, 0, b.Label),
                    "media" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaConsumer, 0, 0,
                            Protocol.MediaKeys.FirstOrDefault(m => m.Name == b.Media).Usage, b.Label),
                    "text" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaText, 0, 0, 0, b.Label),
                    _ => null,
                };
                if (cmd is null) continue;
                await _ble.Write(cmd);
                if (b.Type.Equals("text", StringComparison.OrdinalIgnoreCase))
                    await _ble.Write(Protocol.SetText(prof.Preset, k, b.Target));
                any = true;
            }
        }
        if (any && commit) await _ble.Write(Protocol.Commit());
    }

    async Task PushEyes(string spec, bool persist)
    {
        var cmd = Protocol.SetEyes(spec, persist);
        if (cmd is not null) await _ble.Write(cmd);
    }

    // Live previews from the editor — RAM only, so clicking around costs nothing.
    public void PushEyesLive(string spec)
    {
        var cmd = Protocol.SetEyes(spec, persist: false);
        if (cmd is not null && _ble.IsUp) _ = _ble.Write(cmd);
    }

    public void PushColorLive(int preset, string hex)
    {
        var cmd = Protocol.SetColor(preset, hex);
        if (cmd is not null && _ble.IsUp) _ = _ble.Write(cmd);
    }

    static Task Push(Func<Task> f) => Task.Run(f);

    public void Dispose() { }
}
