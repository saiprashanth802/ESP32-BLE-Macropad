namespace MacroPadDeck;

/// Glue: pad events → actions; foreground changes → pad preset + labels.
///
/// Profile model ("both"): the foreground app picks the profile, but a manual
/// preset switch on the pad wins for StickySeconds before auto-follow resumes.
public sealed class DeckController : IDisposable
{
    readonly IBleLink _ble;
    readonly ProfileStore _store;
    readonly IActionEngine _actions;
    FeishinSource? _feishin;
    ICurrentTrack? _media;
    public void AttachFeishin(FeishinSource? f) => _feishin = f;
    public void AttachMedia(ICurrentTrack m) => _media = m;

    /// The pad's builtin action library, fetched live so the editor's picker
    /// never drifts from the firmware's ACTION_LIB.
    public PadActions? Actions { get; private set; }
    public void AttachActions(PadActions a) => Actions = a;

    /// Local-model rewrite menu. Optional — null when the Windows front-end
    /// hasn't wired one up (the Linux front-end currently doesn't).
    WriteFlow? _write;
    public void AttachWrite(WriteFlow w) => _write = w;
    int _activePreset = -1;               // pad's preset as we last knew it
    DateTime _manualUntil = DateTime.MinValue;
    string _lastExe = "";

    public event Action<string>? StatusChanged;   // tray tooltip / balloon text
    public void RaiseStatus(string s) => StatusChanged?.Invoke(s);

    public DeckController(IBleLink ble, ProfileStore store, IActionEngine actions)
    {
        _ble = ble; _store = store; _actions = actions;
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
                // [fwMajor][keys][presets][activePreset][faceMode][persona] —
                // the face fields let the rewrite menu put back exactly what
                // was there rather than guessing a default.
                if (p.Length >= 6) _write?.NoteFace(p[4], p[5]);
                StatusChanged?.Invoke($"Pad online (fw v{p[0]}, preset {p[3]})");
                _ = Push(async () =>
                {
                    // Colors, eye color and the full key layout are re-pushed on
                    // every connect (RAM only, no NVS wear) — a pad that missed a
                    // commit still wakes up wearing the app's configuration.
                    await PushColors();
                    await PushEyes(_store.Config.EyeColor, persist: false);
                    await PushKeys(_store.Config, commit: false);
                    // After PushKeys, which would otherwise write the write
                    // preset's profile bindings over the menu.
                    if (_write is not null)
                    {
                        await _write.PushMenuKeys();
                        await _write.HealFace();   // undo a face left off by an interrupted session
                    }
                    await PushLabels(_activePreset);
                });
                break;

            case Protocol.EvKey when p.Length >= 2:
            {
                // The rewrite menu owns every key on its own preset, and every
                // key anywhere while it is open.
                if (_write?.TryHandleKey(p[0], p[1]) == true) break;

                var binding = _store.ForPreset(p[0])?.Keys.ElementAtOrDefault(p[1]);
                if (binding is null) break;
                if (binding.Type.Equals("favorite", StringComparison.OrdinalIgnoreCase))
                    _ = Favorite();                          // needs the Feishin socket
                else if (binding.Type.Equals("write", StringComparison.OrdinalIgnoreCase))
                    _write?.Begin(_activePreset);            // remembers where to return
                else
                    ThreadPool.QueueUserWorkItem(_ => _actions.Execute(binding));
                break;
            }

            case Protocol.EvPreset when p.Length >= 1:
                if (p[0] != _activePreset)
                {
                    // The user (or our own echo) switched — user switches stick.
                    // The rewrite menu's own switches are not user intent, so
                    // they must not arm the sticky override.
                    _activePreset = p[0];
                    if (_write?.IsActive != true)
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
        // The rewrite preview takes focus, which is a foreground change — without
        // this the menu would switch presets out from under itself the instant it
        // opened its own window.
        if (_write?.IsActive == true) return;
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
        // The write preset's labels come from styles.json, not profiles.json —
        // a profile reload here would overwrite the style menu with the
        // underlying (unused) bindings.
        if (_write?.OwnsPreset(preset) == true) { await _write.RefreshLabels(); return; }

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
            if (cmd is null) { Diag.Log($"color: preset {prof.Preset} bad hex '{prof.Color}'"); continue; }
            bool ok = await _ble.Write(cmd);
            Diag.Log($"color: preset {prof.Preset} {prof.Color} -> 0x{cmd[3]:X2}{cmd[4]:X2} write={ok}");
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
                    "focusorlaunch" or "open" or "run" or "window" or "favorite" or "write" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaHost, 0, 0, 0, b.Label),
                    "shortcut" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaKey, (byte)b.Mod,
                            Protocol.HidKeys.FirstOrDefault(h => h.Name == b.Key).Hid, 0, b.Label),
                    "media" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaConsumer, 0, 0,
                            Protocol.MediaKeys.FirstOrDefault(m => m.Name == b.Media).Usage, b.Label),
                    "text" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaText, 0, 0, 0, b.Label),
                    // Builtin: the firmware reuses the consumer field as the
                    // action id when kaType is KA_BUILTIN (see HCMD_KEY).
                    "padaction" =>
                        Protocol.SetKey(prof.Preset, k, Protocol.KaBuiltin, 0, 0,
                            (ushort)b.ActionId, b.Label),
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

    /// Favorite the current Feishin track and flash confirmation on the pad,
    /// then restore whatever the status line was showing.
    async Task Favorite()
    {
        if (_feishin is null)
        {
            await _ble.Write(Protocol.SetStatus("NO FEISHIN LINK"));
            StatusChanged?.Invoke("Favorite: Feishin link not configured");
        }
        else
        {
            var (ok, nowFav, title, err) = await _feishin.ToggleFavorite(_media?.CurrentTitle ?? "");
            await _ble.Write(Protocol.SetStatus(
                ok ? (nowFav ? "FAVORITED" : "UNFAVORITED") : err));
            StatusChanged?.Invoke(ok
                ? $"{(nowFav ? "Favorited" : "Unfavorited")}: {title}"
                : $"Favorite: {err}");
        }
        await Task.Delay(1600);
        await _ble.Write(Protocol.SetStatus(""));   // back to the preset name
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
