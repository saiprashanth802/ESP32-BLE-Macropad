using System.Diagnostics;

namespace MacroPadDeck;

/// Linux half of an OTA firmware push: joins the pad's MacroPad-Setup hotspot
/// with nmcli, then hands the transfer to Core's FirmwareClient. The file
/// picker and result dialog live in the GTK layer (LinuxTray).
public static class LinuxFirmwareFlasher
{
    // The pad's Config-Mode SoftAP (see hardware handoff): WPA2 with a fixed key.
    const string ApPassword = "macropad123";

    public static async Task<FirmwareClient.Result> Flash(string binPath, Action<string> status)
    {
        string? bad = FirmwareClient.ValidateSize(new System.IO.FileInfo(binPath).Length);
        if (bad is not null) return new FirmwareClient.Result(false, bad);

        status("Joining pad hotspot…");
        JoinHotspot();

        return await FirmwareClient.Upload(binPath, status);
    }

    static void JoinHotspot()
    {
        // If NetworkManager already has a saved connection this just brings it
        // up; otherwise it associates with the password. Harmless if we're on it.
        try
        {
            var p = Process.Start(new ProcessStartInfo("nmcli",
                $"device wifi connect {FirmwareClient.Ssid} password {ApPassword}")
            { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true });
            p?.WaitForExit(15000);
        }
        catch (Exception ex) { Diag.Log($"firmware: nmcli join failed: {ex.Message}"); }
    }
}
