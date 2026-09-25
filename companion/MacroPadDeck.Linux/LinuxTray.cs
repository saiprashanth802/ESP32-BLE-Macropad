using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using Gtk;

namespace MacroPadDeck;

/// Ayatana AppIndicator tray — the Linux counterpart of the Windows NotifyIcon
/// host. AppIndicator speaks the StatusNotifierItem protocol, so the icon shows
/// on GNOME (with the AppIndicator extension), KDE, and other SNI hosts, unlike
/// the legacy GtkStatusIcon. The popup is a plain GtkSharp menu handed to the
/// indicator by its native handle.
///
/// Owns the GTK main loop; every background callback marshals back onto it via
/// Application.Invoke before touching a widget.
public static class LinuxTray
{
    static MenuItem _statusItem = null!;

    public static void Run(DeckController deck, MediaPusher media, ProfileStore store,
                           LlmClient llm, StyleStore styles)
    {
        // Map the logical "appindicator" name to whichever of the ayatana / old
        // libappindicator sonames is installed.
        NativeLibrary.SetDllImportResolver(typeof(LinuxTray).Assembly, ResolveAppIndicator);

        Application.Init();

        var menu = BuildMenu(deck, media, store, llm, styles);

        IntPtr ind = app_indicator_new("macropad-deck", "input-keyboard", CategoryApplicationStatus);
        app_indicator_set_status(ind, StatusActive);
        app_indicator_set_title(ind, "MacroPad Deck");
        app_indicator_set_menu(ind, menu.Handle);

        deck.StatusChanged += s => Application.Invoke((_, _) =>
            _statusItem.Label = $"● {s}");

        Application.Run();
    }

    static Menu BuildMenu(DeckController deck, MediaPusher media, ProfileStore store,
                          LlmClient llm, StyleStore styles)
    {
        var menu = new Menu();

        // AppIndicator has no tooltip; a disabled header item carries status.
        _statusItem = new MenuItem("● starting…") { Sensitive = false };
        menu.Append(_statusItem);
        menu.Append(new SeparatorMenuItem());

        var nowPlaying = new CheckMenuItem("Now playing → pad") { Active = media.Enabled };
        nowPlaying.Toggled += (_, _) => media.Enabled = nowPlaying.Active;
        menu.Append(nowPlaying);

        var editor = new MenuItem("Open editor");
        editor.Activated += (_, _) => LinuxEditorWindow.Open(deck, store);
        menu.Append(editor);

        menu.Append(OpenFileItem("Edit profiles.json", ProfileStore.FilePath));
        menu.Append(OpenFileItem("Edit styles.json", StyleStore.FilePath));

        // Loading is automatic on first use; unloading is manual so the VRAM
        // comes back on demand (before a game, say) rather than on a timer.
        var loadModel = new MenuItem("Load write model");
        loadModel.Activated += (_, _) => _ = Task.Run(async () =>
        {
            string m = styles.Config.Model;
            deck.RaiseStatus($"Loading {m}…");
            deck.RaiseStatus(await llm.Load(m) ? $"{m} loaded" : $"Could not load {m}");
        });
        menu.Append(loadModel);

        var unloadModel = new MenuItem("Unload write model");
        unloadModel.Activated += (_, _) => _ = Task.Run(async () =>
        {
            string m = styles.Config.Model;
            deck.RaiseStatus(await llm.Unload(m) ? $"{m} unloaded" : $"Could not unload {m}");
        });
        menu.Append(unloadModel);

        var firmware = new MenuItem("Update firmware…");
        firmware.Activated += (_, _) => _ = UpdateFirmware(deck);
        menu.Append(firmware);

        var autostart = new CheckMenuItem("Start on login") { Active = Autostart.IsEnabled };
        autostart.Toggled += (_, _) => Autostart.Set(autostart.Active);
        menu.Append(autostart);

        menu.Append(new SeparatorMenuItem());

        var quit = new MenuItem("Exit");
        quit.Activated += (_, _) => Application.Quit();
        menu.Append(quit);

        menu.ShowAll();      // AppIndicator only renders items that are shown
        return menu;
    }

    /// Hand a config file to the desktop's default editor. The store writes a
    /// default on first construction, so the path always exists by now.
    static MenuItem OpenFileItem(string caption, string path)
    {
        var item = new MenuItem(caption);
        item.Activated += (_, _) =>
        {
            try
            {
                System.Diagnostics.Process.Start(
                    new System.Diagnostics.ProcessStartInfo("xdg-open", path) { UseShellExecute = false });
            }
            catch (Exception ex) { Diag.Log($"tray: xdg-open {path} failed: {ex.Message}"); }
        };
        return item;
    }

    static async Task UpdateFirmware(DeckController deck)
    {
        string? bin = PickFirmwareFile();
        if (bin is null) return;

        var result = await LinuxFirmwareFlasher.Flash(bin, s => deck.RaiseStatus(s));
        Application.Invoke((_, _) =>
        {
            using var dlg = new MessageDialog(null, DialogFlags.Modal,
                result.Ok ? MessageType.Info : MessageType.Error, ButtonsType.Ok, "%s", result.Message)
            { Title = "MacroPad Deck" };
            dlg.Run();
            dlg.Destroy();
        });
    }

    static string? PickFirmwareFile()
    {
        using var dlg = new FileChooserDialog("Pick the firmware .bin to flash", null,
            FileChooserAction.Open, "Cancel", ResponseType.Cancel, "Open", ResponseType.Accept);
        var filter = new FileFilter { Name = "Firmware image (*.bin)" };
        filter.AddPattern("*.bin");
        dlg.AddFilter(filter);

        string? path = dlg.Run() == (int)ResponseType.Accept ? dlg.Filename : null;
        dlg.Destroy();
        return path;
    }

    // ── libayatana-appindicator3 ────────────────────────────────────────────────
    const int CategoryApplicationStatus = 0;   // APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    const int StatusActive = 1;                 // APP_INDICATOR_STATUS_ACTIVE

    static IntPtr ResolveAppIndicator(string name, Assembly asm, DllImportSearchPath? path)
    {
        if (name != "appindicator") return IntPtr.Zero;
        foreach (var so in new[] { "libayatana-appindicator3.so.1", "libappindicator3.so.1" })
            if (NativeLibrary.TryLoad(so, out var h)) return h;
        Diag.Log("tray: no libayatana-appindicator3 / libappindicator3 found — install it (see README)");
        return IntPtr.Zero;
    }

    [DllImport("appindicator")]
    static extern IntPtr app_indicator_new(string id, string iconName, int category);
    [DllImport("appindicator")]
    static extern void app_indicator_set_status(IntPtr self, int status);
    [DllImport("appindicator")]
    static extern void app_indicator_set_title(IntPtr self, string title);
    [DllImport("appindicator")]
    static extern void app_indicator_set_menu(IntPtr self, IntPtr menu);
}

/// XDG autostart entry — the Linux equivalent of the Windows Run registry key.
static class Autostart
{
    static readonly string Dir = Path.Combine(
        Environment.GetEnvironmentVariable("XDG_CONFIG_HOME")
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".config"),
        "autostart");
    static readonly string FilePath = Path.Combine(Dir, "macropad-deck.desktop");

    public static bool IsEnabled => File.Exists(FilePath);

    public static void Set(bool on)
    {
        try
        {
            if (on)
            {
                Directory.CreateDirectory(Dir);
                string exec = Environment.ProcessPath ?? "macropad-deck";
                File.WriteAllText(FilePath,
                    "[Desktop Entry]\n" +
                    "Type=Application\n" +
                    "Name=MacroPad Deck\n" +
                    $"Exec={exec}\n" +
                    "X-GNOME-Autostart-enabled=true\n");
            }
            else if (File.Exists(FilePath))
            {
                File.Delete(FilePath);
            }
        }
        catch (Exception ex) { Diag.Log($"autostart: {ex.Message}"); }
    }
}
