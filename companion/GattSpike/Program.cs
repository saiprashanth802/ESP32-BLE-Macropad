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

// Handler BEFORE subscribe — the device fires its hello the instant the CCCD
// write lands, and we must not race past it.
notifyChar.ValueChanged += (_, e) =>
{
    var r = DataReader.FromBuffer(e.CharacteristicValue);
    byte[] b = new byte[r.UnconsumedBufferLength];
    r.ReadBytes(b);
    Console.WriteLine($"  EVENT: {Convert.ToHexString(b)}");
};

// Force an off→on CCCD transition: a re-run may find it already enabled from
// a previous session, and writing the same value doesn't fire onSubscribe.
await notifyChar.WriteClientCharacteristicConfigurationDescriptorAsync(
    GattClientCharacteristicConfigurationDescriptorValue.None);
var cccd = await notifyChar.WriteClientCharacteristicConfigurationDescriptorAsync(
    GattClientCharacteristicConfigurationDescriptorValue.Notify);
Console.WriteLine($"Subscribe: {cccd}");

async Task<GattCommunicationStatus> WriteCmd(byte[] cmd)
{
    var w = new DataWriter(); w.WriteBytes(cmd);
    return await writeChar!.WriteValueAsync(w.DetachBuffer());
}

if (writeChar is not null)
{
    // 0x82 setStatus "HI FROM PC"
    byte[] status = System.Text.Encoding.ASCII.GetBytes("HI FROM PC");
    byte[] cmd = new byte[2 + status.Length];
    cmd[0] = 0x82; cmd[1] = (byte)status.Length;
    status.CopyTo(cmd, 2);
    Console.WriteLine($"setStatus write: {await WriteCmd(cmd)}");

    // Preset round-trip: setPreset 3 must echo EVENT 03 01 03, then restore 2.
    Console.WriteLine($"setPreset 3:   {await WriteCmd(new byte[] { 0x83, 1, 3 })}");
    await Task.Delay(1500);
    Console.WriteLine($"setPreset 2:   {await WriteCmd(new byte[] { 0x83, 1, 2 })}");
}

Console.WriteLine("Listening for events — press keys on the pad. Ctrl+C to quit.");
await Task.Delay(Timeout.Infinite);
return 0;
