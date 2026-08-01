using System.Runtime.InteropServices;

namespace MacroPadDeck;

/// Reads Windows' master output volume via Core Audio (IAudioEndpointVolume).
///
/// Hand-rolled COM interop rather than a NuGet dependency — this is three
/// interfaces and two calls, and the app otherwise has no third-party packages.
///
/// The default endpoint is re-resolved whenever a read fails, so unplugging a
/// headset (which invalidates the cached endpoint) self-heals on the next poll
/// instead of wedging the readout permanently.
public sealed class CoreAudioVolumeSource : IVolumeSource, IDisposable
{
    // MMDeviceEnumerator
    static readonly Guid CLSID_MMDeviceEnumerator = new("BCDE0395-E52F-467C-8E3D-C4579291692E");
    static readonly Guid IID_IAudioEndpointVolume = new("5CDF2C82-841E-4546-9722-0CF74078229A");

    const int eRender = 0;        // EDataFlow
    const int eMultimedia = 1;    // ERole

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator
    {
        // Vtable order matters — unused entries must still be declared.
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr devices);
        int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        int RegisterEndpointNotificationCallback(IntPtr client);
        int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice
    {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams,
                     [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        int OpenPropertyStore(int access, out IntPtr store);
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        int GetState(out int state);
    }

    [ComImport, Guid("5CDF2C82-841E-4546-9722-0CF74078229A"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioEndpointVolume
    {
        int RegisterControlChangeNotify(IntPtr notify);
        int UnregisterControlChangeNotify(IntPtr notify);
        int GetChannelCount(out uint count);
        int SetMasterVolumeLevel(float level, ref Guid ctx);
        int SetMasterVolumeLevelScalar(float level, ref Guid ctx);
        int GetMasterVolumeLevel(out float level);
        int GetMasterVolumeLevelScalar(out float level);
        int SetChannelVolumeLevel(uint ch, float level, ref Guid ctx);
        int SetChannelVolumeLevelScalar(uint ch, float level, ref Guid ctx);
        int GetChannelVolumeLevel(uint ch, out float level);
        int GetChannelVolumeLevelScalar(uint ch, out float level);
        int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid ctx);
        int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
        // Remaining methods (step info, hardware support, range) are unused.
    }

    IAudioEndpointVolume? _endpoint;
    readonly object _lock = new();

    public VolumeInfo Read()
    {
        lock (_lock)
        {
            if (_endpoint == null && !TryAcquire()) return VolumeInfo.Unknown;

            try
            {
                if (_endpoint!.GetMasterVolumeLevelScalar(out float scalar) != 0) throw new COMException("scalar");
                if (_endpoint!.GetMute(out bool muted) != 0) throw new COMException("mute");
                return new VolumeInfo((int)Math.Round(scalar * 100f), muted);
            }
            catch
            {
                // Endpoint died (device unplugged / default switched). Drop it;
                // the next poll re-acquires rather than failing forever.
                Release();
                return VolumeInfo.Unknown;
            }
        }
    }

    bool TryAcquire()
    {
        try
        {
            Type? t = Type.GetTypeFromCLSID(CLSID_MMDeviceEnumerator);
            if (t == null) return false;
            var enumerator = (IMMDeviceEnumerator?)Activator.CreateInstance(t);
            if (enumerator == null) return false;

            try
            {
                if (enumerator.GetDefaultAudioEndpoint(eRender, eMultimedia, out IMMDevice dev) != 0)
                    return false;
                try
                {
                    Guid iid = IID_IAudioEndpointVolume;
                    if (dev.Activate(ref iid, 1 /* CLSCTX_INPROC_SERVER */, IntPtr.Zero, out object o) != 0)
                        return false;
                    _endpoint = (IAudioEndpointVolume)o;
                    return true;
                }
                finally { Marshal.ReleaseComObject(dev); }
            }
            finally { Marshal.ReleaseComObject(enumerator); }
        }
        catch (Exception ex)
        {
            Diag.Log($"volume: acquire failed {ex.GetType().Name}");
            return false;
        }
    }

    void Release()
    {
        if (_endpoint == null) return;
        try { Marshal.ReleaseComObject(_endpoint); } catch { }
        _endpoint = null;
    }

    public void Dispose() { lock (_lock) Release(); }
}
