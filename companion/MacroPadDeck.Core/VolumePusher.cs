namespace MacroPadDeck;

/// Streams the system's master volume to the pad, which relays it to the
/// encoder puck over ESP-NOW so the dial can show a real level.
///
/// Polls rather than subscribing to endpoint notifications: the callback API
/// is COM-apartment-sensitive and fires on threads we don't own, and a cheap
/// read every 150 ms is far simpler. Only *changes* are written, so a system
/// sitting at one volume produces no BLE traffic at all.
///
/// The poll is deliberately faster than MediaPusher's 1 s. Turning the puck
/// is a round trip — dial → pad → HID → Windows → here → pad → puck — so poll
/// latency lands directly on how quickly the on-screen bar tracks the knob.
public sealed class VolumePusher : IDisposable
{
    readonly IBleLink _ble;
    readonly IVolumeSource _source;
    readonly System.Threading.Timer _poll;

    VolumeInfo _last = new(-2, false);      // -2 so the first real read always pushes
    bool _loggedUnknown;

    public volatile bool Enabled = true;

    public VolumePusher(IBleLink ble, IVolumeSource source)
    {
        _ble = ble;
        _source = source;
        _poll = new System.Threading.Timer(async _ => await Tick(), null,
                                           TimeSpan.FromSeconds(2),
                                           TimeSpan.FromMilliseconds(150));
    }

    static void Log(string m) => Diag.Log($"volume: {m}");

    /// Force the next tick to write even if the level is unchanged. Called
    /// when the pad reconnects — everything the app owns is re-pushed on each
    /// connect, since the pad holds this in RAM only.
    public void Invalidate() => _last = new(-2, false);

    async Task Tick()
    {
        if (!Enabled) return;
        if (!_ble.IsUp) return;

        VolumeInfo v;
        try { v = _source.Read(); }
        catch (Exception ex) { Log($"source EX {ex.GetType().Name} — skipped"); return; }

        if (v.Level == _last.Level && v.Muted == _last.Muted) return;

        // Don't spam the log while a machine sits with no audio endpoint
        if (!v.IsKnown)
        {
            if (!_loggedUnknown) { Log("no audio endpoint — puck will show mode only"); _loggedUnknown = true; }
        }
        else _loggedUnknown = false;

        _last = v;
        await _ble.Write(Protocol.SetVolume(v.Level, v.Muted));
    }

    public void Dispose() => _poll.Dispose();
}
