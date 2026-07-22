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
            if (_activePreset >= 0) _ = PushLabels(_activePreset);
        };
    }

    void OnPadEvent(byte op, byte[] p)
    {
        switch (op)
        {
            case Protocol.EvHello when p.Length >= 4:
                _activePreset = p[3];
                StatusChanged?.Invoke($"Pad online (fw v{p[0]}, preset {p[3]})");
                _ = PushLabels(_activePreset);
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

    static Task Push(Func<Task> f) => Task.Run(f);

    public void Dispose() { }
}
