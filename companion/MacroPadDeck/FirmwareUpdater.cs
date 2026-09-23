using System.Diagnostics;
using System.IO;

namespace MacroPadDeck;

/// Windows front-end for an OTA firmware push. Picks the .bin, asks the pad to
/// drop into config mode over BLE (HCMD_CONFIG), joins its MacroPad-Setup
/// hotspot with netsh, hands the transfer to Core's FirmwareClient, then puts
/// the PC back on the WiFi it was on. Firmware too old for HCMD_CONFIG falls
/// back to asking for config mode by hand. The one-at-a-time guard for the
/// upload itself lives in FirmwareClient.
public static class FirmwareUpdater
{
    static int _busy;

    public static async Task Run(DeckController deck, Action<string> status)
    {
        using var dlg = new OpenFileDialog
        {
            Filter = "Firmware image (*.bin)|*.bin",
            Title = "Pick the firmware .bin to flash",
        };
        if (dlg.ShowDialog() != DialogResult.OK) return;
        string bin = dlg.FileName;

        string? bad = FirmwareClient.ValidateSize(new FileInfo(bin).Length);
        if (bad is not null)
        {
            MessageBox.Show(bad, "MacroPad Deck", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        if (Interlocked.Exchange(ref _busy, 1) == 1) { status("Firmware update already running"); return; }
        string? homeWifi = CurrentSsid();
        try
        {
            // Already in config mode (the user did it on the pad)? Then skip ahead.
            if (!Netsh("wlan show networks").Contains(FirmwareClient.Ssid))
            {
                status("Pad → config mode…");
                await deck.RequestConfigMode();
                if (!await WaitForHotspot(TimeSpan.FromSeconds(25)))
                {
                    if (MessageBox.Show("The pad didn't enter config mode by itself.\n\n" +
                                        "On the pad: hold FN → SETTINGS → K5, then press OK.",
                                        "MacroPad Deck", MessageBoxButtons.OKCancel) != DialogResult.OK)
                        return;
                    if (!await WaitForHotspot(TimeSpan.FromSeconds(60)))
                    {
                        MessageBox.Show("The pad's hotspot never appeared — nothing was flashed.",
                                        "MacroPad Deck", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                        return;
                    }
                }
            }

            status("Joining pad hotspot…");
            // The WiFi profile already exists after the first manual join; this
            // just switches networks. Harmless if we're already on it.
            Netsh($"wlan connect name={FirmwareClient.Ssid}");
            var result = await FirmwareClient.Upload(bin, status);
            Diag.Log($"ota: {Path.GetFileName(bin)} ok={result.Ok} ({result.Message})");
            MessageBox.Show(result.Message, "MacroPad Deck", MessageBoxButtons.OK,
                result.Ok ? MessageBoxIcon.Information : MessageBoxIcon.Error);
        }
        finally
        {
            // Back to the network we were on, whatever happened above
            if (!string.IsNullOrEmpty(homeWifi) && homeWifi != FirmwareClient.Ssid)
            {
                await Task.Delay(3000);
                Netsh($"wlan connect name=\"{homeWifi}\"");
            }
            Interlocked.Exchange(ref _busy, 0);
        }
    }

    static async Task<bool> WaitForHotspot(TimeSpan limit)
    {
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed < limit)
        {
            if (Netsh("wlan show networks").Contains(FirmwareClient.Ssid)) return true;
            await Task.Delay(2000);
        }
        return false;
    }

    static string? CurrentSsid()
    {
        foreach (var line in Netsh("wlan show interfaces").Split('\n'))
        {
            var t = line.Trim();
            // "SSID : x" — but not the "BSSID" line
            if (t.StartsWith("SSID", StringComparison.Ordinal) && t.Contains(':'))
                return t[(t.IndexOf(':') + 1)..].Trim();
        }
        return null;
    }

    static string Netsh(string args)
    {
        try
        {
            using var p = Process.Start(new ProcessStartInfo("netsh", args)
            {
                CreateNoWindow = true, UseShellExecute = false, RedirectStandardOutput = true,
            });
            if (p is null) return "";
            string o = p.StandardOutput.ReadToEnd();
            p.WaitForExit(8000);
            return o;
        }
        catch { return ""; }
    }
}
