using System.Diagnostics;
using WinFormsApp = System.Windows.Forms.Application;

namespace MacroPadDeck;

static class Program
{
    [STAThread]
    static void Main()
    {
        ApplicationConfiguration.Initialize();

        using var store = new ProfileStore();
        ulong addr = Convert.ToUInt64(store.Config.DeviceAddress.Replace(":", ""), 16);
        using var ble = new BleLink(addr);
        using var deck = new DeckController(ble, store);
        using var tray = new TrayContext(deck);

        // Hook must live on the message-pump thread.
        using var fg = new ForegroundWatcher();
        fg.ExeChanged += deck.OnForegroundExe;

        WinFormsApp.Run(tray);
    }
}

sealed class TrayContext : ApplicationContext
{
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunName = "MacroPadDeck";

    readonly NotifyIcon _icon;

    public TrayContext(DeckController deck)
    {
        var menu = new ContextMenuStrip();
        menu.Items.Add("Open editor", null, (_, _) => EditorWindow.Open(deck));
        menu.Items.Add("Edit profiles.json", null, (_, _) =>
            Process.Start(new ProcessStartInfo(ProfileStore.FilePath) { UseShellExecute = true }));

        var autostart = new ToolStripMenuItem("Start with Windows") { CheckOnClick = true, Checked = IsAutostart() };
        autostart.CheckedChanged += (_, _) => SetAutostart(autostart.Checked);
        menu.Items.Add(autostart);

        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, (_, _) => ExitThread());

        _icon = new NotifyIcon
        {
            Icon = System.Drawing.SystemIcons.Application,
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
