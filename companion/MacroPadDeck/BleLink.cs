using System.IO;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Storage.Streams;

namespace MacroPadDeck;

/// The only class that touches WinRT Bluetooth. Owns the connection,
/// subscription and writes; raises plain .NET events for everyone else.
/// Disconnects are normal life (sleep, Easy-Switch, out of range) — this
/// class quietly re-acquires on a timer rather than treating them as errors.
///
/// The pad gives each Easy-Switch slot its OWN BLE address (so each host keeps
/// a clean, independent bond), so a hardcoded address breaks the moment the
/// active slot changes. We therefore DISCOVER the pad among paired devices by
/// name on every acquire; the configured address, if any, is only a fallback
/// pin. See [[macropad-per-slot-address]] in the handoff notes.
public sealed class BleLink : IBleLink
{
    // Matches the pad's DEVICE_NAME / Windows FriendlyName ("ESP32 MacroPad").
    const string PadNameFragment = "MacroPad";
    readonly ulong _hint;   // optional pinned address from config; 0 = pure auto-discovery
    readonly System.Threading.Timer _retry;
    BluetoothLEDevice? _dev;
    GattDeviceService? _svc;
    GattCharacteristic? _evt, _cmd;
    volatile bool _up;
    readonly SemaphoreSlim _gate = new(1, 1);

    public event Action<byte, byte[]>? EventReceived;   // (opcode, payload)
    public event Action<bool>? LinkChanged;             // subscribed / lost

    public bool IsUp => _up;

    static readonly string LogPath = Path.Combine(ProfileStore.Dir, "deck.log");
    /// Shared diagnostic sink so other modules log to the same file.
    public static void Diag(string msg) => Log(msg);
    static void Log(string msg)
    {
        try { File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} {msg}\r\n"); }
        catch { }
    }

    public BleLink(ulong hintAddress = 0)
    {
        _hint = hintAddress;
        _retry = new System.Threading.Timer(async _ => await TryAcquire(), null,
                                            TimeSpan.Zero, TimeSpan.FromSeconds(10));
    }

    async Task TryAcquire()
    {
        if (_up) return;
        if (!await _gate.WaitAsync(0)) return;          // one attempt at a time
        try
        {
            Drop();
            _dev = await ResolveDevice();
            if (_dev is null) { Log("acquire: no paired MacroPad"); return; }
            _dev.ConnectionStatusChanged += OnConnChanged;

            // Full enumeration, not GetGattServicesForUuidAsync — the filtered
            // call throws 0x80070016 ERROR_BAD_COMMAND on this stack.
            var svc = await _dev.GetGattServicesAsync(BluetoothCacheMode.Uncached);
            if (svc.Status != GattCommunicationStatus.Success)
            { Log($"acquire: services {svc.Status}"); return; }
            // Dispose every service we are NOT keeping — WinRT holds a session
            // per undisposed GattDeviceService, and leaked sessions eventually
            // lock the service (GetCharacteristics starts failing AccessDenied).
            GattDeviceService? s = null;
            foreach (var x in svc.Services)
            {
                if (s is null && x.Uuid == Protocol.Service) s = x;
                else x.Dispose();
            }
            if (s is null) { Log("acquire: macropad service absent"); return; }
            _svc = s;

            var chars = await s.GetCharacteristicsAsync(BluetoothCacheMode.Uncached);
            if (chars.Status != GattCommunicationStatus.Success)
            { Log($"acquire: chars {chars.Status}"); return; }
            _evt = chars.Characteristics.FirstOrDefault(c => c.Uuid == Protocol.EvtChar);
            _cmd = chars.Characteristics.FirstOrDefault(c => c.Uuid == Protocol.CmdChar);
            if (_evt is null || _cmd is null) { Log("acquire: evt/cmd char absent"); return; }

            // Handler BEFORE subscribe (the hello races the CCCD status), and
            // cycle the CCCD off→on: it persists per-bond, and rewriting the
            // same value never fires the device's onSubscribe → no hello.
            _evt.ValueChanged += OnValueChanged;
            await _evt.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue.None);
            var st = await _evt.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue.Notify);
            if (st != GattCommunicationStatus.Success) { Log($"acquire: subscribe {st}"); return; }

            Log("acquire: UP");
            _up = true;
            LinkChanged?.Invoke(true);
        }
        catch (Exception ex)
        {
            // 0x8000FFFF etc. — stale cache or mid-reconnect; next tick retries
            Log($"acquire: EX {ex.GetType().Name} 0x{ex.HResult:X8} {ex.Message.Split('\r')[0]}");
        }
        finally { _gate.Release(); }
    }

    /// Find the pad among the machine's PAIRED devices by name, preferring one
    /// that is currently connected (the pad's active slot). This tolerates the
    /// per-slot address change: whichever slot the pad is on, its paired entry
    /// still carries the "ESP32 MacroPad" name. The configured address is only
    /// a last-resort pin — discovery wins so a slot switch needs no config edit.
    /// The host-link service is still verified by the caller (it filters for
    /// Protocol.Service), so a differently-named device can never be adopted.
    async Task<BluetoothLEDevice?> ResolveDevice()
    {
        BluetoothLEDevice? fallback = null;
        try
        {
            var paired = await DeviceInformation.FindAllAsync(
                BluetoothLEDevice.GetDeviceSelectorFromPairingState(true));
            foreach (var di in paired)
            {
                if (di.Name is null ||
                    !di.Name.Contains(PadNameFragment, StringComparison.OrdinalIgnoreCase))
                    continue;
                BluetoothLEDevice? d = null;
                try { d = await BluetoothLEDevice.FromIdAsync(di.Id); } catch { }
                if (d is null) continue;
                // The connected one is the pad's active slot — take it outright.
                if (d.ConnectionStatus == BluetoothConnectionStatus.Connected)
                {
                    fallback?.Dispose();
                    Log($"resolve: connected '{di.Name}' {d.BluetoothAddress:X12}");
                    return d;
                }
                if (fallback is null) fallback = d; else d.Dispose();
            }
        }
        catch (Exception ex) { Log($"resolve: EX {ex.GetType().Name} {ex.Message.Split('\r')[0]}"); }

        if (fallback is not null)
        {
            Log($"resolve: paired '{fallback.Name}' {fallback.BluetoothAddress:X12}");
            return fallback;
        }
        // No paired MacroPad discovered — fall back to a pinned config address.
        if (_hint != 0)
        {
            Log($"resolve: hint {_hint:X12}");
            return await BluetoothLEDevice.FromBluetoothAddressAsync(_hint);
        }
        return null;
    }

    void OnConnChanged(BluetoothLEDevice d, object? _)
    {
        if (d.ConnectionStatus == BluetoothConnectionStatus.Disconnected && _up)
        {
            _up = false;
            LinkChanged?.Invoke(false);
        }
    }

    void OnValueChanged(GattCharacteristic c, GattValueChangedEventArgs e)
    {
        var r = DataReader.FromBuffer(e.CharacteristicValue);
        byte[] b = new byte[r.UnconsumedBufferLength];
        r.ReadBytes(b);
        if (b.Length < 2 || b.Length < 2 + b[1]) return;
        EventReceived?.Invoke(b[0], b[2..(2 + b[1])]);
    }

    public async Task<bool> Write(byte[] cmd)
    {
        var ch = _cmd;
        if (!_up || ch is null) return false;
        try
        {
            var w = new DataWriter(); w.WriteBytes(cmd);
            return await ch.WriteValueAsync(w.DetachBuffer()) == GattCommunicationStatus.Success;
        }
        catch { _up = false; LinkChanged?.Invoke(false); return false; }
    }

    void Drop()
    {
        if (_evt is not null) { try { _evt.ValueChanged -= OnValueChanged; } catch { } }
        _evt = null; _cmd = null;
        if (_svc is not null) { try { _svc.Dispose(); } catch { } }
        _svc = null;
        if (_dev is not null)
        {
            try { _dev.ConnectionStatusChanged -= OnConnChanged; _dev.Dispose(); } catch { }
        }
        _dev = null;
    }

    public void Dispose() { _retry.Dispose(); Drop(); }
}
