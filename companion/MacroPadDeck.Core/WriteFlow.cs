namespace MacroPadDeck;

/// The on-screen review surface for a rewrite. Implemented per platform (WPF
/// on Windows); WriteFlow drives it and never touches UI types itself.
///
/// The pad is the input device and the monitor is the display: a rewritten
/// paragraph will never fit on a 240px screen, but your hand is already on the
/// pad, so accept/reject belongs there rather than on the mouse.
public interface IRewritePreview
{
    void Open(string original, string styleLabel);
    /// Streaming append, so the wait reads as progress rather than a hang.
    void Delta(string text);
    void Done(string body, string subject);
    void Error(string message);
    /// Current text, including any edits the user made in the preview.
    string Text { get; }
    void Close();

    event Action? Accept;
    event Action? Cancel;
    event Action? Again;
}

/// Two-stage pad menu for local-model rewrites.
///
///   trigger key ──► capture selection ──► switch to the write preset
///        stage 1: pick a style          (keys are the style menu)
///        stage 2: ACCEPT / REGEN / CANCEL / BACK   (labels only)
///   ──► paste over the selection ──► switch back
///
/// Stage 2 relabels with SetLabel rather than SetKey on purpose: labels are
/// cosmetic and restorable from StyleStore, whereas key *types* are not
/// restorable for anything the app didn't define.
///
/// Threading: _gate serialises state transitions only. Generation runs
/// detached and re-enters the gate to commit — otherwise a press of CANCEL
/// would queue behind the very request it is trying to cancel.
public sealed class WriteFlow : IDisposable
{
    enum Stage { Off, Picking, Working, Reviewing }

    const int BackKey = 11;
    const int AcceptKey = 0, RegenKey = 1, CancelKey = 2;

    readonly IBleLink _ble;
    readonly StyleStore _styles;
    readonly LlmClient _llm;
    readonly ITextCapture _capture;
    readonly IRewritePreview _preview;

    readonly SemaphoreSlim _gate = new(1, 1);
    readonly System.Threading.Timer _idle;

    Stage _stage = Stage.Off;
    int _returnPreset = -1;
    string _input = "";
    WriteStyle? _style;
    CancellationTokenSource? _gen;
    /// Bumped on every teardown so a late generation or status-clear from a
    /// previous run can tell that it has been superseded.
    int _epoch;

    byte? _knownFaceMode;      // last non-zero mode the pad reported
    byte _reportedMode = 1;    // what it said on the most recent hello
    byte _facePersona;
    /// True only while *we* are the reason the face is off, so a pad the user
    /// deliberately set to OFF is never switched on by us mid-session.
    bool _suppressed;

    byte FaceModeToRestore =>
        _knownFaceMode ?? (byte)Math.Clamp(_styles.Config.RestoreFaceMode, 0, 2);

    /// Told by DeckController from the hello event.
    ///
    /// A reported 0 is deliberately NOT cached as the restore target. This
    /// feature turns the face off itself, so if the app is killed mid-menu the
    /// pad keeps mode 0 in RAM, and the next connect would read it back and
    /// bake OFF in as "what the user wanted" — after which every restore is a
    /// permanent no-op and the pad stays dark with no way back. Only a non-zero
    /// reading is worth remembering.
    public void NoteFace(byte mode, byte persona)
    {
        _facePersona = persona;
        _reportedMode = mode;
        if (mode != 0) _knownFaceMode = mode;
        Log($"pad face mode={mode} ({ModeName(mode)}) persona={persona}" +
            (mode == 0 ? $" — ignoring 0, would restore to {ModeName(FaceModeToRestore)}" : ""));
    }

    /// A pad that comes up with the face OFF is almost always one this feature
    /// suppressed and never got to release. Put it back on connect rather than
    /// leaving a dark pad and no clue why — the same self-healing the rest of
    /// the app does by re-pushing its config on every connect.
    public Task HealFace()
    {
        if (_reportedMode != 0 || IsActive || _suppressed) return Task.CompletedTask;
        byte m = FaceModeToRestore;
        if (m == 0) return Task.CompletedTask;
        Log($"pad came up with the face OFF — healing to {ModeName(m)}");
        return _ble.Write(Protocol.SetFace(m, _facePersona));
    }

    static string ModeName(byte m) => m switch { 0 => "OFF", 1 => "IDLE", 2 => "ALWAYS", _ => "?" };

    /// The menu is only readable with the face off — in ALWAYS mode the pad
    /// returns to the face two seconds after any press, and in IDLE mode after
    /// faceCfg.idleS, so pushed labels would otherwise flash up and vanish.
    ///
    /// Note HCMD_FACE only sets the mode; it does not change currentScreen. If
    /// the pad is sitting ON the face when this lands it stays there until a
    /// key wakes it — the trigger press does that for us, but a menu entered
    /// some other way may need one throwaway press.
    Task FaceOff()
    {
        if (_suppressed) return Task.CompletedTask;
        _suppressed = true;
        Log("face -> OFF");
        return _ble.Write(Protocol.SetFace(0, _facePersona));
    }

    Task FaceRestore()
    {
        if (!_suppressed) return Task.CompletedTask;
        _suppressed = false;
        byte m = FaceModeToRestore;
        Log($"face -> {ModeName(m)} (mode={m})");
        return _ble.Write(Protocol.SetFace(m, _facePersona));
    }

    /// True while the menu is up. DeckController checks this to hold off
    /// profile auto-follow — the preview window taking focus would otherwise
    /// switch presets and yank the pad out of the menu mid-flow.
    public bool IsActive => _stage != Stage.Off;

    public event Action<string>? Status;          // tray tooltip

    public WriteFlow(IBleLink ble, StyleStore styles, LlmClient llm,
                     ITextCapture capture, IRewritePreview preview)
    {
        _ble = ble; _styles = styles; _llm = llm; _capture = capture; _preview = preview;

        _preview.Accept += () => Fire(DoAccept);
        _preview.Cancel += () => Fire(() => End("CANCELLED"));
        _preview.Again  += () => Fire(Regenerate);

        _idle = new System.Threading.Timer(_ => Fire(() => End("TIMED OUT")));
        _styles.Reloaded += () => { if (!IsActive) _ = PushMenuKeys(); };
    }

    int Preset => _styles.Config.WritePreset;
    static void Log(string m) => Diag.Log($"write: {m}");

    /// True when this preset is the menu's, so the caller pushes menu labels
    /// rather than the profile's.
    public bool OwnsPreset(int preset) => Preset >= 0 && preset == Preset;

    /// Label-only refresh for callers that would otherwise push the profile's
    /// labels over the menu. Deliberately not PushMenuKeys: the key *types* are
    /// claimed once per connect, and re-claiming them here doubled the startup
    /// write burst. Cached, so an unchanged menu costs zero writes.
    public Task RefreshLabels() => IsActive ? Task.CompletedTask : PushStageOneLabels();

    /// Run work under the state gate, off the caller's thread.
    void Fire(Func<Task> work) => _ = Post(work);

    async Task Post(Func<Task> work)
    {
        await _gate.WaitAsync().ConfigureAwait(false);
        try { await work(); }
        catch (Exception ex) { Log($"unhandled: {ex.Message}"); }
        finally { _gate.Release(); }
    }

    void Kick() => _idle.Change(
        TimeSpan.FromSeconds(Math.Max(10, _styles.Config.MenuTimeoutSeconds)), Timeout.InfiniteTimeSpan);

    void Sleep() => _idle.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);

    // ---- pad key layout -------------------------------------------------------

    /// Claim the write preset's 12 keys as host-notify keys. Called on every
    /// connect and whenever styles.json changes. No Commit — this is RAM only,
    /// so profiles.json stays the source of truth and releasing the preset
    /// (writePreset = -1) restores the original layout on the next connect.
    public async Task PushMenuKeys()
    {
        int p = Preset;
        if (p < 0 || !_ble.IsUp) return;

        for (int k = 0; k < 12; k++)
        {
            string label = LabelFor(k);
            // SetKey carries the label, so this seeds the cache too.
            _shown[k] = await _ble.Write(Protocol.SetKey(p, k, Protocol.KaHost, 0, 0, 0, label))
                ? label : null;
        }
        Log($"claimed preset {p} for the style menu");
    }

    string LabelFor(int k) => k == BackKey ? "BACK" : _styles.ForKey(k)?.Label ?? "";

    /// What each key is currently displaying, so a stage change writes only the
    /// keys that actually differ. Measured on real hardware, a BLE label write
    /// costs 30-60 ms — a blind 12-key relabel is half a second of watching
    /// labels fill in one at a time. null means unknown (write failed or link
    /// dropped), which forces a rewrite next time.
    readonly string?[] _shown = new string?[12];

    async Task SetLabel(int key, string label)
    {
        if (_shown[key] == label) return;
        _shown[key] = await _ble.Write(Protocol.SetLabel(Preset, key, label)) ? label : null;
    }

    async Task PushStageOneLabels()
    {
        if (Preset < 0) return;
        for (int k = 0; k < 12; k++) await SetLabel(k, LabelFor(k));
    }

    async Task PushStageTwoLabels()
    {
        await SetLabel(AcceptKey, "ACCEPT");
        await SetLabel(RegenKey,  "REGEN");
        await SetLabel(CancelKey, "CANCEL");
        for (int k = 3; k < BackKey; k++) await SetLabel(k, "");
    }

    Task Say(string s) => _ble.IsUp ? _ble.Write(Protocol.SetStatus(Trim(s, 23))) : Task.CompletedTask;

    // ---- entry points ---------------------------------------------------------

    /// A key bound to type "write" was pressed. currentPreset is where the pad
    /// should return to when the menu closes.
    public void Begin(int currentPreset) => Fire(() => BeginCore(currentPreset));

    async Task BeginCore(int currentPreset)
    {
        if (IsActive) return;
        if (Preset < 0) { Status?.Invoke("Write menu disabled (writePreset = -1)"); return; }

        if (!await Prepare()) return;

        _returnPreset = currentPreset != Preset ? currentPreset : -1;
        _stage = Stage.Picking;
        await FaceOff();
        await _ble.Write(Protocol.SetPreset(Preset));
        await PushStageOneLabels();
        await Say("PICK STYLE");
        Kick();
    }

    /// Capture the selection and confirm a model server is there. Returns false
    /// with the reason already on the pad if the flow can't usefully start.
    async Task<bool> Prepare()
    {
        await Say("READING SELECTION");
        string? text = await _capture.Capture();
        if (string.IsNullOrWhiteSpace(text))
        {
            Status?.Invoke("Write: nothing selected");
            await Say("NOTHING SELECTED");
            ClearStatusLater(_epoch);
            return false;
        }

        if (!await _llm.IsUp())
        {
            Log("aborting — model server did not answer");
            Status?.Invoke("Write: Ollama is not running");
            await Say("LLM OFFLINE");
            _capture.Abandon();
            ClearStatusLater(_epoch);
            return false;
        }

        Log($"captured {text.Length} chars");
        _input = text;
        return true;
    }

    /// A pad key arrived. Returns true if the flow owns it, so the caller skips
    /// its normal binding dispatch.
    public bool TryHandleKey(int preset, int key)
    {
        if (!IsActive && preset != Preset) return false;
        Fire(() => OnKey(preset, key));
        return true;
    }

    async Task OnKey(int preset, int key)
    {
        // Entering by switching to the write preset by hand rather than through
        // a trigger key — the capture happens at the style press instead.
        if (_stage == Stage.Off)
        {
            if (preset != Preset || key == BackKey) return;
            if (_styles.ForKey(key) is null) return;
            if (!await Prepare()) return;
            _returnPreset = -1;
            _stage = Stage.Picking;
            await FaceOff();
        }

        Kick();

        switch (_stage)
        {
            case Stage.Picking:
                if (key == BackKey) { await End("CANCELLED"); return; }
                if (_styles.ForKey(key) is not { } style) return;
                _style = style;
                StartGeneration();
                return;

            case Stage.Working:
                // Only cancelling is meaningful mid-generation.
                if (key is BackKey or CancelKey) await End("CANCELLED");
                return;

            case Stage.Reviewing:
                switch (key)
                {
                    case AcceptKey: await DoAccept(); return;
                    case RegenKey:  await Regenerate(); return;
                    case CancelKey: await End("CANCELLED"); return;
                    case BackKey:                          // back to the style picker
                        _preview.Close();
                        _stage = Stage.Picking;
                        await PushStageOneLabels();
                        await Say("PICK STYLE");
                        return;
                }
                return;
        }
    }

    // ---- generation -----------------------------------------------------------

    /// Called under the gate; the actual request runs detached so that CANCEL
    /// can be serviced while it is in flight.
    void StartGeneration()
    {
        if (_style is not { } style) return;
        _stage = Stage.Working;

        _gen?.Dispose();
        _gen = new CancellationTokenSource(
            TimeSpan.FromSeconds(Math.Max(30, _styles.Config.RequestTimeoutSeconds)));

        int epoch = _epoch;
        var ct = _gen.Token;
        string original = _input;
        _ = Task.Run(() => RunGeneration(style, original, epoch, ct));
    }

    async Task RunGeneration(WriteStyle style, string original, int epoch, CancellationToken ct)
    {
        string model = _styles.ModelFor(style);
        _preview.Open(original, style.Label);

        if (_styles.Config.AutoLoad && !await _llm.IsLoaded(model))
        {
            Status?.Invoke($"Write: loading {model}");
            await Say("LOADING MODEL");
            if (!await _llm.Load(model))
            {
                Fire(() => Fail(epoch, $"Could not load {model}"));
                return;
            }
        }

        await Say(style.Label + "...");

        var result = await _llm.Run(style, model, original,
                                    delta => { if (epoch == _epoch) _preview.Delta(delta); }, ct);

        Fire(() => Commit(epoch, result));
    }

    async Task Commit(int epoch, LlmResult result)
    {
        if (epoch != _epoch || _stage != Stage.Working) return;   // superseded or cancelled
        if (!result.Ok) { await Fail(epoch, result.Error); return; }

        _preview.Done(result.Text, result.Subject);
        if (result.Subject.Length > 0) await _capture.ToClipboard(result.Subject);

        _stage = Stage.Reviewing;
        await PushStageTwoLabels();
        await Say(result.Subject.Length > 0 ? "SUBJECT ON CLIPBOARD" : "REVIEW ON SCREEN");
        Kick();
    }

    Task Regenerate()
    {
        if (_stage is not (Stage.Reviewing or Stage.Working)) return Task.CompletedTask;
        StartGeneration();
        return Task.CompletedTask;
    }

    async Task Fail(int epoch, string why)
    {
        if (epoch != _epoch) return;
        Log($"failed: {why}");
        _preview.Error(why);
        Status?.Invoke($"Write: {why}");
        await Say(why.ToUpperInvariant());
        EndLater(epoch, 1800);
    }

    async Task DoAccept()
    {
        if (_stage != Stage.Reviewing) return;
        string text = _preview.Text;
        _preview.Close();

        if (string.IsNullOrWhiteSpace(text)) { await End("NOTHING TO PASTE"); return; }

        await Say("PASTING");
        bool ok = await _capture.Replace(text);
        Status?.Invoke(ok ? "Write: pasted" : "Write: paste failed — window lost focus");
        await End(ok ? "PASTED" : "PASTE FAILED");
    }

    // ---- teardown -------------------------------------------------------------

    async Task End(string? finalStatus)
    {
        if (_stage == Stage.Off) return;
        Sleep();
        _epoch++;                       // invalidate anything still in flight

        _gen?.Cancel();
        _stage = Stage.Off;
        _style = null;
        _input = "";
        _preview.Close();
        _capture.Abandon();

        await PushStageOneLabels();                        // menu back to stage 1
        if (_returnPreset >= 0 && _returnPreset != Preset)
            await _ble.Write(Protocol.SetPreset(_returnPreset));
        _returnPreset = -1;
        await FaceRestore();                               // face comes back last

        if (finalStatus is not null) await Say(finalStatus);
        ClearStatusLater(_epoch);
    }

    void EndLater(int epoch, int delayMs) => _ = Task.Run(async () =>
    {
        await Task.Delay(delayMs);
        if (epoch == _epoch) Fire(() => End(null));
    });

    /// Let a confirmation sit for a beat, then hand the status line back to the
    /// preset name — same pattern the favorite action uses. Detached so it
    /// never holds the gate, and epoch-guarded so a stale timer can't wipe the
    /// status of a run that started in the meantime.
    void ClearStatusLater(int epoch) => _ = Task.Run(async () =>
    {
        await Task.Delay(1600);
        if (epoch == _epoch && !IsActive) await Say("");
    });

    static string Trim(string s, int max) => s.Length <= max ? s : s[..max];

    public void Dispose()
    {
        _idle.Dispose();
        _gen?.Dispose();
        _gate.Dispose();
    }
}
