using System.Diagnostics;
using System.IO;

namespace MacroPadDeck;

/// Windows front-end for an OTA firmware push. Picks the .bin, joins the pad's
/// MacroPad-Setup hotspot with netsh, then hands the transfer to Core's
/// FirmwareClient. The one-at-a-time guard lives in FirmwareClient.
public static class FirmwareUpdater
{
    public static async Task Run(Action<string> status)
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

        status("Joining pad hotspot…");
        // The WiFi profile already exists after the first manual join; this just
        // switches networks. Harmless if we're already on it.
        Process.Start(new ProcessStartInfo("netsh", $"wlan connect name={FirmwareClient.Ssid}")
        { CreateNoWindow = true, UseShellExecute = false })?.WaitForExit(5000);

        var result = await FirmwareClient.Upload(bin, status);
        MessageBox.Show(result.Message, "MacroPad Deck", MessageBoxButtons.OK,
            result.Ok ? MessageBoxIcon.Information : MessageBoxIcon.Error);
    }
}
