using System.Diagnostics;

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

        Application.Run(tray);
    }
}

sealed class TrayContext : ApplicationContext
{
    readonly NotifyIcon _icon;

    public TrayContext(DeckController deck)
    {
        var menu = new ContextMenuStrip();
        menu.Items.Add("Edit profiles", null, (_, _) =>
            Process.Start(new ProcessStartInfo(ProfileStore.FilePath) { UseShellExecute = true }));
        menu.Items.Add("Open profiles folder", null, (_, _) =>
            Process.Start(new ProcessStartInfo(ProfileStore.Dir) { UseShellExecute = true }));
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, (_, _) => ExitThread());

        _icon = new NotifyIcon
        {
            Icon = System.Drawing.SystemIcons.Application,
            Text = "MacroPad Deck — starting…",
            Visible = true,
            ContextMenuStrip = menu,
        };

        deck.StatusChanged += s =>
        {
            string txt = $"MacroPad Deck — {s}";
            _icon.Text = txt.Length > 63 ? txt[..63] : txt;   // NotifyIcon hard limit
        };
    }

    protected override void ExitThreadCore()
    {
        _icon.Visible = false;
        _icon.Dispose();
        base.ExitThreadCore();
    }
}
