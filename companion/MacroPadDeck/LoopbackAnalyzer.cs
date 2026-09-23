using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace MacroPadDeck;

/// Listens to whatever Windows is playing (WASAPI loopback on the default
/// output) and feeds it to a BeatTracker. The audio is reduced to a handful of
/// numbers in memory and never stored or sent anywhere — only tempo, phase and
/// a loudness level ever reach the pad.
///
/// Loopback delivers nothing while the output is silent, so "no data for a
/// second" is how silence shows up; BeatTracker.Read reports it as inactive.
/// A 5 s watchdog rebinds when the default device changes (headphones
/// plugged in) or the capture stops on its own (device removed).
public sealed class LoopbackAnalyzer : IAudioAnalyzer
{
    readonly object _gate = new();
    readonly System.Threading.Timer _watchdog;
    WasapiLoopbackCapture? _cap;
    BeatTracker? _bt;
    string _deviceId = "";
    float[] _mono = Array.Empty<float>();
    bool _loggedFormat;
    volatile bool _disposed;

    public LoopbackAnalyzer()
    {
        _watchdog = new System.Threading.Timer(_ => Watch(), null, TimeSpan.Zero, TimeSpan.FromSeconds(5));
    }

    static void Log(string m) => Diag.Log($"loopback: {m}");

    static string DefaultDeviceId(out MMDevice? dev)
    {
        dev = null;
        try
        {
            using var en = new MMDeviceEnumerator();
            dev = en.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            return dev.ID;
        }
        catch { return ""; }               // no output device at all
    }

    void Watch()
    {
        if (_disposed) return;
        string id = DefaultDeviceId(out var dev);
        WasapiLoopbackCapture? old = null;
        try { lock (_gate)
        {
            if (_cap is not null && id == _deviceId) { dev?.Dispose(); return; }
            old = DetachLocked();
            if (dev is null) return;
            try
            {
                var cap = new WasapiLoopbackCapture(dev);
                var fmt = cap.WaveFormat;
                var bt = new BeatTracker(fmt.SampleRate);
                cap.DataAvailable += (_, e) => OnData(cap, bt, fmt, e);
                cap.RecordingStopped += (_, e) =>
                {
                    if (e.Exception is not null) Log($"stopped: {e.Exception.Message}");
                    lock (_gate) if (_cap == cap) { _cap = null; _deviceId = ""; }
                };
                cap.StartRecording();
                _cap = cap; _bt = bt; _deviceId = id; _loggedFormat = false;
                Log($"listening on '{dev.FriendlyName}' {fmt.SampleRate} Hz {fmt.Channels} ch {fmt.Encoding}/{fmt.BitsPerSample}");
            }
            catch (Exception ex) { Log($"start failed: {ex.GetType().Name} {ex.Message}"); }
        } }
        finally { Shutdown(old); }
    }

    void OnData(WasapiLoopbackCapture cap, BeatTracker bt, WaveFormat fmt, WaveInEventArgs e)
    {
        double endMs = BeatTracker.NowMs();
        int ch = Math.Max(1, fmt.Channels);
        int frames;
        if (fmt.Encoding == WaveFormatEncoding.IeeeFloat && fmt.BitsPerSample == 32)
        {
            frames = e.BytesRecorded / (4 * ch);
            Ensure(frames);
            var src = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, float>(
                          e.Buffer.AsSpan(0, frames * 4 * ch));
            for (int f = 0; f < frames; f++)
            {
                float s = 0;
                for (int c = 0; c < ch; c++) s += src[f * ch + c];
                _mono[f] = s / ch;
            }
        }
        else if (fmt.Encoding == WaveFormatEncoding.Pcm && fmt.BitsPerSample == 16)
        {
            frames = e.BytesRecorded / (2 * ch);
            Ensure(frames);
            var src = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, short>(
                          e.Buffer.AsSpan(0, frames * 2 * ch));
            for (int f = 0; f < frames; f++)
            {
                float s = 0;
                for (int c = 0; c < ch; c++) s += src[f * ch + c] / 32768f;
                _mono[f] = s / ch;
            }
        }
        else
        {
            // Shared-mode loopback is float32 in practice; anything else is logged once
            if (!_loggedFormat) { Log($"unsupported format {fmt.Encoding}/{fmt.BitsPerSample}"); _loggedFormat = true; }
            return;
        }
        bt.Process(_mono.AsSpan(0, frames), endMs);
    }

    void Ensure(int frames) { if (_mono.Length < frames) _mono = new float[frames * 2]; }

    public AudioFeatures Read()
    {
        BeatTracker? bt;
        lock (_gate) bt = _bt;
        try { return bt?.Read(BeatTracker.NowMs()) ?? AudioFeatures.Silent; }
        catch { return AudioFeatures.Silent; }
    }

    // Detach under the lock, stop outside it. Dispose joins the capture thread,
    // and that thread takes _gate in RecordingStopped — disposing while holding
    // _gate deadlocked the first build (the probe never exited).
    WasapiLoopbackCapture? DetachLocked()
    {
        var cap = _cap;
        _cap = null; _bt = null; _deviceId = "";
        return cap;
    }

    static void Shutdown(WasapiLoopbackCapture? cap)
    {
        if (cap is null) return;
        try { cap.StopRecording(); } catch { }
        try { cap.Dispose(); } catch { }
    }

    public void Dispose()
    {
        _disposed = true;
        _watchdog.Dispose();
        WasapiLoopbackCapture? cap;
        lock (_gate) cap = DetachLocked();
        Shutdown(cap);
    }
}
