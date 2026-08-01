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

        FeishinSource? feishin = store.Config.FeishinUrl.Length > 0
            ? new FeishinSource(store.Config.FeishinUrl, store.Config.FeishinUser, store.Config.FeishinPassword)
            : null;
        deck.AttachFeishin(feishin);

        using var media = new MediaPusher(ble, new MprisMediaSource(), feishin,
                                          src => store.Config.IsMusicSource(src))
                          { Enabled = store.Config.NowPlaying };
        deck.AttachMedia(media);

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

        using var fg = new X11ForegroundWatcher();
        fg.ExeChanged += deck.OnForegroundExe;

        LinuxTray.Run(deck, media, store);
    }
}
