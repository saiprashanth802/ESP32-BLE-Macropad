using Linux.Bluetooth;
using Linux.Bluetooth.Extensions;

namespace MacroPadDeck;

/// The only class that touches BlueZ. Owns the connection, notification
/// subscription and writes; raises the same plain IBleLink events the Windows
/// WinRT link does, so DeckController/MediaPusher are unaware of the stack.
///
/// Disconnects are normal life (sleep, Easy-Switch, out of range) — this class
/// quietly re-acquires on a timer rather than treating them as errors, exactly
/// like the Windows side.
public sealed class BlueZBleLink : IBleLink
{
    readonly string _address;                 // "84:1F:E8:2B:33:4A"
    readonly string _svcUuid, _evtUuid, _cmdUuid;
    readonly System.Threading.Timer _retry;
    readonly SemaphoreSlim _gate = new(1, 1);
    readonly Dictionary<string, object> _noOpts = new();

    Adapter? _adapter;
    Device? _dev;
    GattCharacteristic? _evt, _cmd;
    volatile bool _up;

    public event Action<byte, byte[]>? EventReceived;   // (opcode, payload)
    public event Action<bool>? LinkChanged;             // subscribed / lost
    public bool IsUp => _up;

    static void Log(string m) => Diag.Log($"ble: {m}");

    public BlueZBleLink(ulong address)
    {
        _address = FormatAddress(address);
        // BlueZ wants the full 128-bit UUID as a lowercase string; the protocol
        // Guids stringify to exactly that.
        _svcUuid = Protocol.Service.ToString();
        _evtUuid = Protocol.EvtChar.ToString();
        _cmdUuid = Protocol.CmdChar.ToString();
        _retry = new System.Threading.Timer(async _ => await TryAcquire(), null,
                                             TimeSpan.Zero, TimeSpan.FromSeconds(10));
    }

    static string FormatAddress(ulong addr)
    {
        var b = new byte[6];
        for (int i = 0; i < 6; i++) b[5 - i] = (byte)(addr >> (8 * i));
        return string.Join(":", b.Select(x => x.ToString("X2")));
    }

    async Task TryAcquire()
    {
        if (_up) return;
        if (!await _gate.WaitAsync(0)) return;          // one attempt at a time
        try
        {
            Drop();

            _adapter ??= (await BlueZManager.GetAdaptersAsync()).FirstOrDefault();
            if (_adapter is null) { Log("no bluetooth adapter"); return; }
            try { await _adapter.SetPoweredAsync(true); } catch { /* already on / no perms */ }

            // GetDeviceAsync resolves a cached/known device by address. If BlueZ
            // has never seen the pad, kick a short discovery and let the next
            // tick pick it up.
            _dev = await _adapter.GetDeviceAsync(_address);
            if (_dev is null)
            {
                try { await _adapter.StartDiscoveryAsync(); } catch { }
                Log($"device {_address} not known yet — scanning");
                return;
            }

            await _dev.ConnectAsync();
            // Wait for the GATT DB to resolve before asking for services.
            await _dev.WaitForPropertyValueAsync("ServicesResolved", true, TimeSpan.FromSeconds(15));

            var svc = await _dev.GetServiceAsync(_svcUuid);
            if (svc is null) { Log("macropad service absent"); return; }

            _evt = await svc.GetCharacteristicAsync(_evtUuid);
            _cmd = await svc.GetCharacteristicAsync(_cmdUuid);
            if (_evt is null || _cmd is null) { Log("evt/cmd characteristic absent"); return; }

            // Handler before StartNotify so the hello can't race the subscription.
            _evt.Value += OnValue;
            await _evt.StartNotifyAsync();
            _dev.Disconnected += OnDisconnected;

            Log("UP");
            _up = true;
            LinkChanged?.Invoke(true);
        }
        catch (Exception ex)
        {
            // Mid-reconnect, adapter busy, or the pad asleep — next tick retries.
            Log($"acquire EX {ex.GetType().Name}: {ex.Message.Split('\n')[0]}");
        }
        finally { _gate.Release(); }
    }

    Task OnValue(GattCharacteristic sender, GattCharacteristicValueEventArgs e)
    {
        byte[] b = e.Value;
        if (b.Length >= 2 && b.Length >= 2 + b[1])
        {
            Log($"evt op=0x{b[0]:X2} len={b[1]} [{Convert.ToHexString(b, 2, b[1])}]");
            EventReceived?.Invoke(b[0], b[2..(2 + b[1])]);
        }
        else
            Log($"evt malformed ({b.Length} bytes)");
        return Task.CompletedTask;
    }

    Task OnDisconnected(Device sender, BlueZEventArgs e)
    {
        if (_up)
        {
            _up = false;
            LinkChanged?.Invoke(false);
        }
        return Task.CompletedTask;
    }

    public async Task<bool> Write(byte[] cmd)
    {
        var ch = _cmd;
        if (!_up || ch is null) return false;
        try
        {
            await ch.WriteValueAsync(cmd, _noOpts);
            return true;
        }
        catch { _up = false; LinkChanged?.Invoke(false); return false; }
    }

    void Drop()
    {
        if (_evt is not null) { try { _evt.Value -= OnValue; } catch { } }
        _evt = null; _cmd = null;
        if (_dev is not null)
        {
            try { _dev.Disconnected -= OnDisconnected; } catch { }
            try { _dev.Dispose(); } catch { }
        }
        _dev = null;
    }

    public void Dispose() { _retry.Dispose(); Drop(); }
}
