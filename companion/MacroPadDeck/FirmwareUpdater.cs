using System.Diagnostics;
using System.IO;
using System.Net.Http;

namespace MacroPadDeck;

/// OTA firmware push via the pad's Config Mode.
/// Flow: user puts the pad in Settings → CONFIG (it raises the
/// MacroPad-Setup hotspot), picks a .bin here, we join the hotspot and POST
/// it to /api/update. The pad flashes the inactive slot and reboots itself.
public static class FirmwareUpdater
{
    const string Ssid = "MacroPad-Setup";
    const string UpdateUrl = "http://192.168.4.1/api/update";
    const string InfoUrl = "http://192.168.4.1/api/info";

    // Two concurrent POSTs to /api/update would interleave into the same OTA
    // partition and brick the image — one flash at a time, always.
    static int _busy;

    public static async Task Run(Action<string> status)
    {
        if (Interlocked.Exchange(ref _busy, 1) == 1)
        {
            MessageBox.Show("A firmware update is already in progress.",
                            "MacroPad Deck", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        try { await RunCore(status); }
        finally { Interlocked.Exchange(ref _busy, 0); }
    }

    static async Task RunCore(Action<string> status)
    {
        using var dlg = new OpenFileDialog
        {
            Filter = "Firmware image (*.bin)|*.bin",
            Title = "Pick the firmware .bin to flash",
        };
        if (dlg.ShowDialog() != DialogResult.OK) return;
        string bin = dlg.FileName;
        long size = new FileInfo(bin).Length;
        if (size < 100_000 || size > 1_500_000)
        {
            MessageBox.Show($"That file is {size / 1024} KB — a v5 image is ~1.3 MB. Wrong file?",
                            "MacroPad Deck", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        status("Joining pad hotspot…");
        // The WiFi profile already exists after the first manual join; this
        // just switches networks. Harmless if we're already on it.
        Process.Start(new ProcessStartInfo("netsh", $"wlan connect name={Ssid}")
        { CreateNoWindow = true, UseShellExecute = false })?.WaitForExit(5000);

        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(150) };

        // Wait for the pad to answer (up to ~20 s of association + DHCP)
        bool reachable = false;
        for (int i = 0; i < 10 && !reachable; i++)
        {
            try { await http.GetStringAsync(InfoUrl); reachable = true; }
            catch { await Task.Delay(2000); }
        }
        if (!reachable)
        {
            status("Pad not reachable");
            MessageBox.Show("Couldn't reach the pad at 192.168.4.1.\n\nIs it in Settings → CONFIG mode, " +
                            "and is this PC on the MacroPad-Setup network?",
                            "MacroPad Deck", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        status("Uploading firmware…");
        try
        {
            using var form = new MultipartFormDataContent();
            using var fs = File.OpenRead(bin);
            var part = new StreamContent(fs);
            part.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/octet-stream");
            form.Add(part, "f", "firmware.bin");
            var resp = await http.PostAsync(UpdateUrl, form);
            string body = await resp.Content.ReadAsStringAsync();
            bool ok = resp.IsSuccessStatusCode && body.Contains("true");
            status(ok ? "Firmware flashed — pad rebooting" : "Firmware upload failed");
            MessageBox.Show(ok
                ? "Flashed. The pad is rebooting into the new firmware —\nthe hotspot disappearing is the confirmation."
                : $"Upload failed: {(int)resp.StatusCode} {body}\nThe running firmware is untouched.",
                "MacroPad Deck", MessageBoxButtons.OK,
                ok ? MessageBoxIcon.Information : MessageBoxIcon.Error);
        }
        catch (Exception ex)
        {
            status("Firmware upload failed");
            MessageBox.Show($"Upload failed: {ex.Message}\nThe running firmware is untouched.",
                            "MacroPad Deck", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
