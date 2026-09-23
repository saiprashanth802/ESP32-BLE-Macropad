using System.Diagnostics;
using System.IO;

namespace MacroPadDeck;

/// Tray → Face style. The look (bot / classic) is fixed per firmware build —
/// every scheme for switching it at runtime failed on the ESP32's heap — so
/// switching looks means flashing the other image. This drives the whole
/// thing unattended: pad into config mode over BLE, join its hotspot, OTA the
/// image, then put the PC back on the WiFi it was on.
///
/// Images live in %USERPROFILE%\MacroPadDeck\firmware\ as
/// macropad_v5_bot.bin / macropad_v5_classic.bin (built with FACE_LOOK_BOT=1/0).
public static class FaceStyleFlasher
{
    public static readonly string Dir = Path.Combine(ProfileStore.Dir, "firmware");
    public static string ImageFor(bool bot) =>
        Path.Combine(Dir, bot ? "macropad_v5_bot.bin" : "macropad_v5_classic.bin");

    static int _busy;

    public static async Task Run(DeckController deck, bool bot, Action<string> status)
    {
        string look = bot ? "bot" : "classic";
        if (deck.PadIsBot == bot) { status($"Face is already {look}"); return; }

        string bin = ImageFor(bot);
        if (!File.Exists(bin))
        {
            MessageBox.Show($"No {look} firmware image at\n{bin}", "MacroPad Deck",
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        string? bad = FirmwareClient.ValidateSize(new FileInfo(bin).Length);
        if (bad is not null) { MessageBox.Show(bad, "MacroPad Deck"); return; }

        if (MessageBox.Show($"Switch the pad to the {look} face?\n\n" +
                            "It goes into config mode and is flashed with the " + look +
                            " build (about a minute). The PC briefly joins the pad's " +
                            "hotspot and then returns to your WiFi.",
                            "MacroPad Deck", MessageBoxButtons.OKCancel,
                            MessageBoxIcon.Question) != DialogResult.OK) return;

        if (Interlocked.Exchange(ref _busy, 1) == 1) { status("Face switch already running"); return; }
        string? homeWifi = CurrentSsid();
        try
        {
            status("Pad → config mode…");
            await deck.RequestConfigMode();
            if (!await WaitForHotspot(TimeSpan.FromSeconds(25)))
            {
                // Pre-0x90 firmware ignores the request; fall back to the menu
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

            status("Joining pad hotspot…");
            Netsh($"wlan connect name={FirmwareClient.Ssid}");
            var result = await FirmwareClient.Upload(bin, status);
            Diag.Log($"face: flashed {look} image ok={result.Ok} ({result.Message})");
            status(result.Ok ? $"Face: {look} — pad rebooting" : "Face switch failed");
            if (!result.Ok)
                MessageBox.Show(result.Message, "MacroPad Deck", MessageBoxButtons.OK, MessageBoxIcon.Error);
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
