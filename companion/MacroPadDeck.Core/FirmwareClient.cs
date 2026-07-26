using System.IO;
using System.Net.Http;

namespace MacroPadDeck;

/// The transport half of an OTA firmware push, shared across platforms. Joining
/// the pad's MacroPad-Setup hotspot (netsh on Windows, nmcli on Linux) and the
/// file-picker / message-box UI stay in the front-end; this just talks HTTP to
/// the pad once the machine is on its network.
public static class FirmwareClient
{
    public const string Ssid = "MacroPad-Setup";
    const string UpdateUrl = "http://192.168.4.1/api/update";
    const string InfoUrl   = "http://192.168.4.1/api/info";

    // Two concurrent POSTs to /api/update would interleave into the same OTA
    // partition and brick the image — one flash at a time, always.
    static int _busy;

    /// A basic sanity range for a v5 image (~1.3 MB). Returns a human message
    /// when the file is obviously wrong, else null.
    public static string? ValidateSize(long bytes) =>
        bytes is < 100_000 or > 1_500_000
            ? $"That file is {bytes / 1024} KB — a v5 image is ~1.3 MB. Wrong file?"
            : null;

    public sealed record Result(bool Ok, string Message);

    /// Poll /api/info until the pad answers (association + DHCP take a moment),
    /// then POST the image. `status` reports progress for the tray tooltip.
    /// Guarded so a second call while one is running returns Busy immediately.
    public static async Task<Result> Upload(string binPath, Action<string> status)
    {
        if (Interlocked.Exchange(ref _busy, 1) == 1)
            return new Result(false, "A firmware update is already in progress.");
        try { return await UploadCore(binPath, status); }
        finally { Interlocked.Exchange(ref _busy, 0); }
    }

    static async Task<Result> UploadCore(string binPath, Action<string> status)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(150) };

        status("Waiting for pad…");
        bool reachable = false;
        for (int i = 0; i < 10 && !reachable; i++)
        {
            try { await http.GetStringAsync(InfoUrl); reachable = true; }
            catch { await Task.Delay(2000); }
        }
        if (!reachable)
        {
            status("Pad not reachable");
            return new Result(false,
                "Couldn't reach the pad at 192.168.4.1.\n\nIs it in Settings → CONFIG mode, " +
                "and is this machine on the MacroPad-Setup network?");
        }

        status("Uploading firmware…");
        try
        {
            using var form = new MultipartFormDataContent();
            using var fs = File.OpenRead(binPath);
            var part = new StreamContent(fs);
            part.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/octet-stream");
            form.Add(part, "f", "firmware.bin");
            var resp = await http.PostAsync(UpdateUrl, form);
            string body = await resp.Content.ReadAsStringAsync();
            bool ok = resp.IsSuccessStatusCode && body.Contains("true");
            status(ok ? "Firmware flashed — pad rebooting" : "Firmware upload failed");
            return new Result(ok, ok
                ? "Flashed. The pad is rebooting into the new firmware —\nthe hotspot disappearing is the confirmation."
                : $"Upload failed: {(int)resp.StatusCode} {body}\nThe running firmware is untouched.");
        }
        catch (Exception ex)
        {
            status("Firmware upload failed");
            return new Result(false, $"Upload failed: {ex.Message}\nThe running firmware is untouched.");
        }
    }
}
