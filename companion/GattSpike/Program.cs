// GattSpike — GO/NO-GO test for the MacroPad companion architecture.
//
// Spike A: can a user-mode Windows app enumerate GATT services on a bonded
//          BLE HID keyboard that the inbox HOGP driver has claimed?
// Spike B: (after the firmware gains the custom service) does the "macropad"
//          service appear, do notifications arrive on keypress, do writes land?
//
// Usage:  GattSpike [hexAddress]      default 841FE82B334A (Windows-reported
//                                     BLE address; base MAC is ...48)

using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

const string MacropadSvcPrefix = "6d616372-6f70-6164";   // ASCII "macropad"

ulong address = 0x841FE82B334A;
if (args.Length > 0)
    address = Convert.ToUInt64(args[0].Replace(":", ""), 16);

Console.WriteLine($"Connecting to {address:X12} ...");
var dev = await BluetoothLEDevice.FromBluetoothAddressAsync(address);
if (dev is null)
{
    Console.WriteLine("FAIL: FromBluetoothAddressAsync returned null (device unreachable / not advertising / not bonded).");
    return 1;
}

Console.WriteLine($"Name:              {dev.Name}");
Console.WriteLine($"ConnectionStatus:  {dev.ConnectionStatus}");
Console.WriteLine($"Paired:            {dev.DeviceInformation.Pairing.IsPaired}");
Console.WriteLine();

var svcResult = await dev.GetGattServicesAsync(BluetoothCacheMode.Uncached);
Console.WriteLine($"GetGattServicesAsync: {svcResult.Status}");
if (svcResult.Status != GattCommunicationStatus.Success)
{
    Console.WriteLine("FAIL: cannot enumerate services.");
    return 1;
}

GattCharacteristic? notifyChar = null, writeChar = null;

foreach (var svc in svcResult.Services)
{
    Console.WriteLine($"\nService {svc.Uuid}");
    var chResult = await svc.GetCharacteristicsAsync(BluetoothCacheMode.Uncached);
    if (chResult.Status != GattCommunicationStatus.Success)
    {
        Console.WriteLine($"  (characteristics: {chResult.Status})");
        continue;
    }
    foreach (var ch in chResult.Characteristics)
    {
        Console.WriteLine($"  Char {ch.Uuid}  [{ch.CharacteristicProperties}]");
        if (svc.Uuid.ToString().StartsWith(MacropadSvcPrefix))
        {
            if (ch.CharacteristicProperties.HasFlag(GattCharacteristicProperties.Notify))
                notifyChar = ch;
            if (ch.CharacteristicProperties.HasFlag(GattCharacteristicProperties.Write) ||
                ch.CharacteristicProperties.HasFlag(GattCharacteristicProperties.WriteWithoutResponse))
                writeChar = ch;
        }
    }
}

Console.WriteLine();
if (notifyChar is null)
{
    Console.WriteLine("Spike A complete. No macropad service found (expected before the firmware change).");
    Console.WriteLine("PASS if the HID (00001812-...) and Device Information services are listed above.");
    return 0;
}

// ── Spike B ─────────────────────────────────────────────────────────────
Console.WriteLine("macropad service found — Spike B: subscribing to events...");
var cccd = await notifyChar.WriteClientCharacteristicConfigurationDescriptorAsync(
    GattClientCharacteristicConfigurationDescriptorValue.Notify);
Console.WriteLine($"Subscribe: {cccd}");

notifyChar.ValueChanged += (_, e) =>
{
    var r = DataReader.FromBuffer(e.CharacteristicValue);
    byte[] b = new byte[r.UnconsumedBufferLength];
    r.ReadBytes(b);
    Console.WriteLine($"  EVENT: {Convert.ToHexString(b)}");
};

if (writeChar is not null)
{
    // 0x82 setStatus "HI FROM PC"
    byte[] status = System.Text.Encoding.ASCII.GetBytes("HI FROM PC");
    byte[] cmd = new byte[2 + status.Length];
    cmd[0] = 0x82; cmd[1] = (byte)status.Length;
    status.CopyTo(cmd, 2);
    var w = new DataWriter(); w.WriteBytes(cmd);
    var ws = await writeChar.WriteValueAsync(w.DetachBuffer());
    Console.WriteLine($"setStatus write: {ws}");
}

Console.WriteLine("Listening for events — press keys on the pad. Ctrl+C to quit.");
await Task.Delay(Timeout.Infinite);
return 0;
