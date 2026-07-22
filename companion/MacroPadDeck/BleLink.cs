using System.IO;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

namespace MacroPadDeck;

/// The only class that touches WinRT Bluetooth. Owns the connection,
/// subscription and writes; raises plain .NET events for everyone else.
/// Disconnects are normal life (sleep, Easy-Switch, out of range) — this
/// class quietly re-acquires on a timer rather than treating them as errors.
public sealed class BleLink : IDisposable
{
    readonly ulong _address;
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

    public BleLink(ulong address)
    {
        _address = address;
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
            _dev = await BluetoothLEDevice.FromBluetoothAddressAsync(_address);
            if (_dev is null) { Log("acquire: device null"); return; }
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
