namespace MacroPadDeck;

/// One entry from the pad's builtin action library (firmware ACTION_LIB).
public readonly record struct PadAction(int Id, string Label)
{
    public override string ToString() => Label;
}

/// Fetches and caches the pad's builtin action list over the host link.
///
/// The list lives in the firmware's ACTION_LIB and is pulled a page at a time
/// rather than duplicated here — an 86-entry copy in C# would silently drift
/// the moment someone adds an action to the sketch, and the failure mode is a
/// key bound to the wrong action, which is nasty to diagnose.
///
/// Paging is request/response: ask for page N, the pad notifies HEV_ACTIONS
/// with that page, and we ask for the next until totalPages is reached.
public sealed class PadActions
{
    const int EntrySize = 11;          // [id lo][id hi][label 9]

    readonly IBleLink _ble;
    readonly List<PadAction> _items = new();
    readonly object _lock = new();

    int _expectedPages = -1;
    TaskCompletionSource<bool>? _pending;

    /// Fires once a full fetch completes, so the editor can refresh its picker.
    public event Action? Updated;

    public PadActions(IBleLink ble)
    {
        _ble = ble;
        _ble.EventReceived += OnEvent;
    }

    static void Log(string m) => Diag.Log($"actions: {m}");

    /// Snapshot of what has been fetched. Empty until a fetch succeeds.
    public IReadOnlyList<PadAction> Items
    {
        get { lock (_lock) return _items.ToArray(); }
    }

    public bool HasData { get { lock (_lock) return _items.Count > 0; } }

    /// Pull the whole library. Safe to call repeatedly; returns false if the
    /// link is down or the pad stops replying partway.
    public async Task<bool> Fetch(CancellationToken ct = default)
    {
        if (!_ble.IsUp) return false;

        lock (_lock) { _items.Clear(); _expectedPages = -1; }

        for (int page = 0; page < 64; page++)      // 64 pages = 512 actions, ample
        {
            var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            _pending = tcs;

            if (!await _ble.Write(Protocol.GetActions(page)))
            {
                Log($"write failed at page {page}");
                return false;
            }

            // The pad answers from its loop task, so a page is a few ms away.
            // A generous timeout still fails fast if the firmware predates this.
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(ct, timeout.Token);
            try
            {
                await tcs.Task.WaitAsync(linked.Token);
            }
            catch (OperationCanceledException)
            {
                Log($"timeout on page {page} — old firmware without HCMD_ACTIONS?");
                return false;
            }
            finally { _pending = null; }

            int total;
            lock (_lock) total = _expectedPages;
            if (total <= 0 || page >= total - 1) break;
        }

        int n;
        lock (_lock) n = _items.Count;
        Log($"fetched {n} builtin actions");
        Updated?.Invoke();
        return n > 0;
    }

    void OnEvent(byte op, byte[] payload)
    {
        if (op != Protocol.EvActions) return;
        if (payload.Length < 3) return;

        int total = payload[1];
        int count = payload[2];
        if (payload.Length < 3 + count * EntrySize) return;

        lock (_lock)
        {
            _expectedPages = total;
            for (int i = 0; i < count; i++)
            {
                int o = 3 + i * EntrySize;
                int id = payload[o] | (payload[o + 1] << 8);
                string label = System.Text.Encoding.ASCII
                    .GetString(payload, o + 2, 9).TrimEnd('\0', ' ');
                if (label.Length > 0) _items.Add(new PadAction(id, label));
            }
        }

        _pending?.TrySetResult(true);
    }
}
