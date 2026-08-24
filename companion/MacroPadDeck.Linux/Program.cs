namespace MacroPadDeck;

static class LinuxProgram
{
    static void Main(string[] args)
    {
        ProfileStore.DeckSampleKeys = LinuxDefaults.DeckSampleKeys;
        using var store = new ProfileStore();
        ulong addr = Convert.ToUInt64(store.Config.DeviceAddress.Replace(":", ""), 16);

        using var ble = new BlueZBleLink(addr);
        using var deck = new DeckController(ble, store, new LinuxActionEngine());
        deck.AttachActions(new PadActions(ble));

        // Local-model rewrite menu. Unlike the Windows build there is no STA /
        // message-pump constraint on construction — every GTK touch inside the
        // preview marshals onto the main loop, which LinuxTray starts below.
        using var styles = new StyleStore();
        using var llm = new LlmClient(() => styles.Config);
        var preview = new GtkRewritePreview();
        using var capture = new LinuxTextCapture(() => preview.FocusIsSafe);
        using var write = new WriteFlow(ble, styles, llm, capture, preview);
        write.Status += deck.RaiseStatus;
        deck.AttachWrite(write);

        FeishinSource? feishin = store.Config.FeishinUrl.Length > 0
            ? new FeishinSource(store.Config.FeishinUrl, store.Config.FeishinUser, store.Config.FeishinPassword)
            : null;
        deck.AttachFeishin(feishin);

        using var media = new MediaPusher(ble, new MprisMediaSource(), feishin,
                                          src => store.Config.IsMusicSource(src))
                          { Enabled = store.Config.NowPlaying };
        deck.AttachMedia(media);

        // Real system volume for the encoder puck's readout. Harmless when no
        // puck exists — the pad just stores the level and never relays it.
        using var volSource = new PulseVolumeSource();
        using var volume = new VolumePusher(ble, volSource);
        ble.LinkChanged += up => { if (up) volume.Invalidate(); };

        // `--editor`: open the GTK editor directly, without the tray. Handy where
        // the SNI tray isn't up yet (e.g. GNOME/Wayland before the AppIndicator
        // extension is registered). BLE still connects, so Save pushes live if the
        // pad is in range; otherwise it syncs on next connect.
        if (args.Contains("--editor"))
        {
            Gtk.Application.Init();
            LinuxEditorWindow.Open(deck, store, quitOnClose: true);
            Gtk.Application.Run();
            return;
        }

        // Foreground-follow: X11's _NET_ACTIVE_WINDOW is blind to native-Wayland
        // windows, so under Wayland read the focused window's wm_class from the
        // Focused Window D-Bus GNOME extension instead.
        bool wayland = string.Equals(
            Environment.GetEnvironmentVariable("XDG_SESSION_TYPE"), "wayland",
            StringComparison.OrdinalIgnoreCase);
        using IForegroundWatcher fg = wayland
            ? new GnomeWaylandForegroundWatcher()
            : new X11ForegroundWatcher();
        fg.ExeChanged += deck.OnForegroundExe;

        LinuxTray.Run(deck, media, store, llm, styles);
        feishin?.Dispose();
    }
}
