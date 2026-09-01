using System.Diagnostics;
using WinFormsApp = System.Windows.Forms.Application;

namespace MacroPadDeck;

static class Program
{
    [STAThread]
    static void Main()
    {
        ApplicationConfiguration.Initialize();

        ProfileStore.DeckSampleKeys = WindowsDefaults.DeckSampleKeys;
        using var store = new ProfileStore();
        ulong addr = Convert.ToUInt64(store.Config.DeviceAddress.Replace(":", ""), 16);
        using var ble = new BleLink(addr);
        using var deck = new DeckController(ble, store, new ActionEngine());
        deck.AttachActions(new PadActions(ble));

        // Local-model rewrite menu. Both the capture control's handle and the
        // preview's dispatcher must be created on this (STA, pumped) thread.
        using var styles = new StyleStore();
        using var llm = new LlmClient(() => styles.Config);
        using var capture = new WindowsTextCapture();
        using var write = new WriteFlow(ble, styles, llm, capture, new RewritePreviewWindow());
        write.Status += deck.RaiseStatus;
        deck.AttachWrite(write);

        // One Feishin link, two consumers: now-playing fallback + favorite control
        FeishinSource? feishin = store.Config.FeishinUrl.Length > 0
            ? new FeishinSource(store.Config.FeishinUrl, store.Config.FeishinUser,
                                store.Config.FeishinPassword)
            : null;
        deck.AttachFeishin(feishin);

        using var media = new MediaPusher(ble, new SmtcMediaSource(), feishin,
                                          src => store.Config.IsMusicSource(src))
                          { Enabled = store.Config.NowPlaying };
        deck.AttachMedia(media);

        // Real system volume for the encoder puck's readout. Harmless when no
        // puck exists — the pad just stores the level and never relays it.
        using var volSource = new CoreAudioVolumeSource();
        using var volume = new VolumePusher(ble, volSource);
        ble.LinkChanged += up => { if (up) volume.Invalidate(); };

        using var tray = new TrayContext(deck, media, llm, styles);

        // Hook must live on the message-pump thread.
        using var fg = new ForegroundWatcher();
        fg.ExeChanged += deck.OnForegroundExe;

        WinFormsApp.Run(tray);
        feishin?.Dispose();
    }
}

sealed class TrayContext : ApplicationContext
{
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunName = "MacroPadDeck";

    readonly NotifyIcon _icon;

    public TrayContext(DeckController deck, MediaPusher media, LlmClient llm, StyleStore styles)
    {
        var menu = new ContextMenuStrip();

        var nowPlaying = new ToolStripMenuItem("Now playing → pad") { CheckOnClick = true, Checked = media.Enabled };
        nowPlaying.CheckedChanged += (_, _) => media.Enabled = nowPlaying.Checked;
        menu.Items.Add(nowPlaying);
        menu.Items.Add("Open editor", null, (_, _) => EditorWindow.Open(deck));
        menu.Items.Add("Edit profiles.json", null, (_, _) =>
            Process.Start(new ProcessStartInfo(ProfileStore.FilePath) { UseShellExecute = true }));
        menu.Items.Add("Edit styles.json", null, (_, _) =>
            Process.Start(new ProcessStartInfo(StyleStore.FilePath) { UseShellExecute = true }));

        // Loading is automatic on first use; unloading is manual so the VRAM
        // comes back on demand (before a game, say) rather than on a timer.
        menu.Items.Add("Load write model", null, async (_, _) =>
        {
            string m = styles.Config.Model;
            deck.RaiseStatus($"Loading {m}…");
            deck.RaiseStatus(await llm.Load(m) ? $"{m} loaded" : $"Could not load {m}");
        });
        menu.Items.Add("Unload write model", null, async (_, _) =>
        {
            string m = styles.Config.Model;
            deck.RaiseStatus(await llm.Unload(m) ? $"{m} unloaded" : $"Could not unload {m}");
        });

        menu.Items.Add("Update firmware…", null, async (_, _) =>
            await FirmwareUpdater.Run(s => deck.RaiseStatus(s)));

        var autostart = new ToolStripMenuItem("Start with Windows") { CheckOnClick = true, Checked = IsAutostart() };
        autostart.CheckedChanged += (_, _) => SetAutostart(autostart.Checked);
        menu.Items.Add(autostart);

        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, (_, _) => ExitThread());

        // The exe carries deck.ico (csproj ApplicationIcon) — reuse it here so
        // the tray, taskbar and Explorer all show the same identity.
        System.Drawing.Icon appIcon;
        try { appIcon = System.Drawing.Icon.ExtractAssociatedIcon(WinFormsApp.ExecutablePath)!; }
        catch { appIcon = System.Drawing.SystemIcons.Application; }

        _icon = new NotifyIcon
        {
            Icon = appIcon,
            Text = "MacroPad Deck — starting…",
            Visible = true,
            ContextMenuStrip = menu,
        };
        _icon.DoubleClick += (_, _) => EditorWindow.Open(deck);

        deck.StatusChanged += s =>
        {
            string txt = $"MacroPad Deck — {s}";
            _icon.Text = txt.Length > 63 ? txt[..63] : txt;   // NotifyIcon hard limit
        };
    }

    static bool IsAutostart()
    {
        using var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunKey);
        return k?.GetValue(RunName) is not null;
    }

    static void SetAutostart(bool on)
    {
        using var k = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RunKey);
        if (on) k.SetValue(RunName, $"\"{WinFormsApp.ExecutablePath}\"");
        else k.DeleteValue(RunName, throwOnMissingValue: false);
    }

    protected override void ExitThreadCore()
    {
        _icon.Visible = false;
        _icon.Dispose();
        base.ExitThreadCore();
    }
}
