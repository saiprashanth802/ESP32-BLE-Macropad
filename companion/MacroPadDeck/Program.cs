using System.Diagnostics;
using WinFormsApp = System.Windows.Forms.Application;

namespace MacroPadDeck;

static class Program
{
    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    static extern bool AttachConsole(int pid);

    [STAThread]
    static void Main()
    {
        var args = Environment.GetCommandLineArgs();

        // `--audio-probe [s]`: print what the loopback analyzer hears, then exit.
        // Needs no pad and touches nothing else — for tuning the beat tracker.
        if (args.Contains("--audio-probe"))
        {
            AttachConsole(-1);
            int secs = args.SkipWhile(a => a != "--audio-probe").Skip(1)
                           .Select(a => int.TryParse(a, out int n) ? n : 0).FirstOrDefault();
            using var probe = new LoopbackAnalyzer();
            for (int i = 0; i < (secs > 0 ? secs : 30); i++)
            {
                Thread.Sleep(1000);
                var f = probe.Read();
                Console.WriteLine($"{i + 1,3}s active={f.Active} loud={f.Loudness:F2} bright={f.Brightness:F2} "
                                + $"bpm={f.Bpm:F1} conf={f.Confidence} "
                                + $"sinceBeat={(f.Active && f.Bpm > 0 ? BeatTracker.NowMs() - f.LastBeatMs : 0):F0}ms");
            }
            return;
        }

        ApplicationConfiguration.Initialize();

        ProfileStore.DeckSampleKeys = WindowsDefaults.DeckSampleKeys;
        using var store = new ProfileStore();
        // deviceAddress is now an optional pin/override. BleLink auto-discovers
        // the paired MacroPad by name, so a slot switch that changes the pad's
        // BLE address no longer needs a config edit + restart. Empty = pure auto.
        string cfgAddr = (store.Config.DeviceAddress ?? "").Replace(":", "").Trim();
        ulong hint = cfgAddr.Length == 0 ? 0 : Convert.ToUInt64(cfgAddr, 16);
        using var ble = new BleLink(hint);
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

        // Face mood: music tags + system audio + app context → HCMD_MOOD/BEAT.
        // Audio capture is only started when moods.json allows it.
        using var moods = new MoodStore();
        using var loopback = moods.Config.Audio ? new LoopbackAnalyzer() : null;
        using var mood = new MoodEngine(ble, moods, media, feishin, loopback);
        ble.LinkChanged += up => { if (up) mood.Invalidate(); };
        // `--mood-set v a`: pin one point of the mood map (−100..100 each)
        int ms = Array.IndexOf(args, "--mood-set");
        if (ms >= 0 && ms + 2 < args.Length &&
            int.TryParse(args[ms + 1], out int pv) && int.TryParse(args[ms + 2], out int pa))
            mood.Pin = (pv, pa);

        using var tray = new TrayContext(deck, media, llm, styles, mood);

        // Hook must live on the message-pump thread.
        using var fg = new ForegroundWatcher();
        fg.ExeChanged += deck.OnForegroundExe;
        fg.ExeChanged += mood.OnForegroundExe;

        // `--editor`: open the editor straight away and exit when it closes. For UI
        // work and screenshots — the tray icon lives in Windows 11's hidden overflow,
        // which UI automation cannot reach, so this is the only scriptable way in.
        if (args.Contains("--editor"))
        {
            EditorWindow.Open(deck);
            EditorWindow.Current!.Closed += (_, _) => tray.ExitThread();
        }


        WinFormsApp.Run(tray);
        feishin?.Dispose();
    }
}

sealed class TrayContext : ApplicationContext
{
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunName = "MacroPadDeck";

    readonly NotifyIcon _icon;

    public TrayContext(DeckController deck, MediaPusher media, LlmClient llm, StyleStore styles,
                       MoodEngine mood)
    {
        var menu = new ContextMenuStrip();

        var nowPlaying = new ToolStripMenuItem("Now playing → pad") { CheckOnClick = true, Checked = media.Enabled };
        nowPlaying.CheckedChanged += (_, _) => media.Enabled = nowPlaying.Checked;
        menu.Items.Add(nowPlaying);
        // Off releases the face at once (weight 0) — the pad goes back to its own mood
        var moodItem = new ToolStripMenuItem("Mood from music && apps") { CheckOnClick = true, Checked = mood.Enabled };
        moodItem.CheckedChanged += (_, _) => mood.Enabled = moodItem.Checked;
        menu.Items.Add(moodItem);
        menu.Items.Add("Edit moods.json", null, (_, _) =>
            Process.Start(new ProcessStartInfo(MoodStore.FilePath) { UseShellExecute = true }));

        // Face look = which firmware build is on the pad; picking the other one
        // flashes it (FaceStyleFlasher). Ticks follow the pad's hello; before any
        // hello (or on pre-v2 firmware) neither is ticked.
        var faceMenu = new ToolStripMenuItem("Face style");
        var botItem = new ToolStripMenuItem("Bot (LED)");
        var classicItem = new ToolStripMenuItem("Classic");
        botItem.Click += async (_, _) => await FaceStyleFlasher.Run(deck, true, deck.RaiseStatus);
        classicItem.Click += async (_, _) => await FaceStyleFlasher.Run(deck, false, deck.RaiseStatus);
        faceMenu.DropDownItems.AddRange(new ToolStripItem[] { botItem, classicItem });
        faceMenu.DropDownOpening += (_, _) =>
        {
            botItem.Checked = deck.PadIsBot == true;
            classicItem.Checked = deck.PadIsBot == false;
        };
        menu.Items.Add(faceMenu);
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
