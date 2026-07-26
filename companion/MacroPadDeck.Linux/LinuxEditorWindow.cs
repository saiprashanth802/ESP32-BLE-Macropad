using System.IO;
using System.Text.Json;
using Gtk;

namespace MacroPadDeck;

/// GTK profile editor — the Linux counterpart of the WPF EditorWindow. Edits a
/// private copy of profiles.json (same on-disk format, shared serializer), and
/// on Save writes the file and pushes the app-managed keys, colours and eye
/// colour to the pad via DeckController.ApplyPadConfig.
///
/// A deliberately practical UI: per-preset name/accent/eye colour and a 12-key
/// grid. Rich per-type detail (window op, shortcut key, media action) is chosen
/// from a context combo; app/file/run targets are plain text. Macro editing
/// stays in the web config, exactly as on Windows.
public sealed class LinuxEditorWindow
{
    static readonly string[] Types =
        { "none", "focusOrLaunch", "open", "run", "window", "shortcut", "media", "text", "favorite" };
    static readonly string[] WindowOps = { "left", "right", "maximize", "minimize", "nextMonitor" };

    static LinuxEditorWindow? _open;

    readonly DeckController _deck;
    readonly ProfileStore _store;
    readonly Window _win;
    DeckConfig _cfg;
    int _curPreset;

    // Header widgets
    readonly ComboBoxText _presetCombo = new();
    readonly Entry _nameEntry = new();
    readonly Entry _colorEntry = new();
    readonly Entry _eyeEntry = new();
    readonly Label _hint = new("");

    readonly KeyRow[] _rows = new KeyRow[12];

    public static void Open(DeckController deck, ProfileStore store, bool quitOnClose = false)
    {
        // GTK is single-threaded; the tray calls this from the main loop already,
        // but a background caller must marshal.
        Application.Invoke((_, _) =>
        {
            if (_open is not null) { _open._win.Present(); return; }
            _open = new LinuxEditorWindow(deck, store);
            // A standalone (`--editor`) launch has no tray to fall back to, so
            // closing the window must end the GTK loop and the process.
            if (quitOnClose) _open._win.DeleteEvent += (_, _) => Application.Quit();
            _open._win.ShowAll();
        });
    }

    LinuxEditorWindow(DeckController deck, ProfileStore store)
    {
        _deck = deck;
        _store = store;
        _cfg = LoadCfg();
        EnsureEightPresets(_cfg);

        _win = new Window("MacroPad Deck — Editor");
        _win.SetDefaultSize(720, 640);
        _win.DeleteEvent += (_, _) => { _open = null; };

        var root = new Box(Orientation.Vertical, 8) { BorderWidth = 12 };

        // ── header: preset picker + name/colour/eye ──────────────────────────
        foreach (var p in _cfg.Profiles.OrderBy(p => p.Preset))
            _presetCombo.AppendText($"{p.Preset}: {p.Name}");
        _presetCombo.Active = 0;
        _presetCombo.Changed += (_, _) => SwitchPreset(_presetCombo.Active);

        root.PackStart(Labeled("Preset", _presetCombo), false, false, 0);
        root.PackStart(Labeled("Name", _nameEntry), false, false, 0);
        root.PackStart(Labeled("Accent colour (#RRGGBB)", _colorEntry), false, false, 0);
        root.PackStart(Labeled("Eye colour (#RRGGBB or 'preset')", _eyeEntry), false, false, 0);
        _eyeEntry.Text = _cfg.EyeColor;

        root.PackStart(new Separator(Orientation.Horizontal), false, false, 4);

        // ── key grid ─────────────────────────────────────────────────────────
        var grid = new Grid { ColumnSpacing = 6, RowSpacing = 4 };
        string[] heads = { "Key", "Type", "Label", "Target", "Args", "Detail", "Mods" };
        for (int c = 0; c < heads.Length; c++)
            grid.Attach(new Label(heads[c]) { Halign = Align.Start }, c, 0, 1, 1);

        for (int k = 0; k < 12; k++)
        {
            _rows[k] = new KeyRow(k);
            _rows[k].Attach(grid, k + 1);
        }

        var scroll = new ScrolledWindow();
        scroll.SetPolicy(PolicyType.Never, PolicyType.Automatic);
        scroll.Add(grid);
        root.PackStart(scroll, true, true, 0);

        // ── footer: save ─────────────────────────────────────────────────────
        var save = new Button("Save & push to pad");
        save.Clicked += (_, _) => Save();
        var footer = new Box(Orientation.Horizontal, 8);
        footer.PackStart(save, false, false, 0);
        footer.PackStart(_hint, false, false, 0);
        root.PackStart(footer, false, false, 0);

        _win.Add(root);

        _curPreset = PresetAt(0);
        LoadPreset(_curPreset);
    }

    static DeckConfig LoadCfg()
    {
        try { return JsonSerializer.Deserialize<DeckConfig>(File.ReadAllText(ProfileStore.FilePath), ProfileStore.JsonOpts) ?? new(); }
        catch { return new DeckConfig(); }
    }

    static void EnsureEightPresets(DeckConfig cfg)
    {
        for (int i = 0; i < 8; i++)
            if (cfg.Profiles.All(p => p.Preset != i))
                cfg.Profiles.Add(new Profile { Preset = i, Name = $"PRESET{i}" });
    }

    int PresetAt(int comboIndex) =>
        _cfg.Profiles.OrderBy(p => p.Preset).ElementAt(comboIndex).Preset;

    Profile Cur => _cfg.Profiles.First(p => p.Preset == _curPreset);

    void SwitchPreset(int comboIndex)
    {
        FlushPreset();
        _curPreset = PresetAt(comboIndex);
        LoadPreset(_curPreset);
    }

    void LoadPreset(int preset)
    {
        var p = _cfg.Profiles.First(x => x.Preset == preset);
        _nameEntry.Text = p.Name;
        _colorEntry.Text = p.Color;
        while (p.Keys.Count < 12) p.Keys.Add(new KeyBinding());
        for (int k = 0; k < 12; k++) _rows[k].Load(p.Keys[k]);
    }

    void FlushPreset()
    {
        var p = Cur;
        p.Name = _nameEntry.Text;
        p.Color = _colorEntry.Text;
        while (p.Keys.Count < 12) p.Keys.Add(new KeyBinding());
        for (int k = 0; k < 12; k++) p.Keys[k] = _rows[k].Read();
        _cfg.EyeColor = _eyeEntry.Text;
    }

    void Save()
    {
        FlushPreset();
        try
        {
            File.WriteAllText(ProfileStore.FilePath, JsonSerializer.Serialize(_cfg, ProfileStore.JsonOpts));
            _ = _deck.ApplyPadConfig(_cfg);
            _hint.Text = $"Saved {DateTime.Now:HH:mm:ss} — pushed to pad.";
        }
        catch (Exception ex)
        {
            _hint.Text = $"Save failed: {ex.Message}";
            Diag.Log($"editor: save failed: {ex.Message}");
        }
    }

    static Widget Labeled(string caption, Widget field)
    {
        var box = new Box(Orientation.Horizontal, 8);
        box.PackStart(new Label(caption) { Halign = Align.Start, WidthChars = 26, Xalign = 0 }, false, false, 0);
        box.PackStart(field, true, true, 0);
        return box;
    }

    /// One editable key row: type + label + target + args + a type-aware detail
    /// combo + modifier text (for shortcuts).
    sealed class KeyRow
    {
        readonly int _k;
        readonly Label _lbl;
        readonly ComboBoxText _type = new();
        readonly Entry _label = new() { WidthChars = 9 };
        readonly Entry _target = new();
        readonly Entry _args = new() { WidthChars = 10 };
        readonly ComboBoxText _detail = new();
        readonly Entry _mods = new() { WidthChars = 12, PlaceholderText = "ctrl,shift" };

        public KeyRow(int k)
        {
            _k = k;
            _lbl = new Label($"K{k + 1}") { Halign = Align.Start };
            foreach (var t in Types) _type.AppendText(t);
            _type.Active = 0;
            _type.Changed += (_, _) => PopulateDetail(_type.ActiveText ?? "none", null);
            _target.WidthChars = 16;
        }

        public void Attach(Grid grid, int row)
        {
            grid.Attach(_lbl, 0, row, 1, 1);
            grid.Attach(_type, 1, row, 1, 1);
            grid.Attach(_label, 2, row, 1, 1);
            grid.Attach(_target, 3, row, 1, 1);
            grid.Attach(_args, 4, row, 1, 1);
            grid.Attach(_detail, 5, row, 1, 1);
            grid.Attach(_mods, 6, row, 1, 1);
        }

        public void Load(KeyBinding b)
        {
            _type.Active = Math.Max(0, Array.FindIndex(Types, t => t.Equals(b.Type, StringComparison.OrdinalIgnoreCase)));
            _label.Text = b.Label;
            _args.Text = b.Args;
            _mods.Text = ModsToText(b.Mod);

            string type = b.Type.ToLowerInvariant();
            _target.Text = type is "shortcut" or "media" or "window" ? "" : b.Target;
            string detailVal = type switch
            {
                "window"   => b.Target,
                "shortcut" => b.Key,
                "media"    => b.Media,
                _          => "",
            };
            PopulateDetail(_type.ActiveText ?? "none", detailVal);
        }

        public KeyBinding Read()
        {
            string type = _type.ActiveText ?? "none";
            var kb = new KeyBinding { Type = type, Label = _label.Text };
            switch (type.ToLowerInvariant())
            {
                case "focusorlaunch":
                case "open":
                case "run":
                    kb.Target = _target.Text; kb.Args = _args.Text; break;
                case "text":
                    kb.Target = _target.Text; break;
                case "window":
                    kb.Target = _detail.ActiveText ?? ""; break;
                case "shortcut":
                    kb.Key = _detail.ActiveText ?? ""; kb.Mod = ParseMods(_mods.Text); break;
                case "media":
                    kb.Media = _detail.ActiveText ?? ""; break;
                case "favorite":
                    break;
                default:
                    kb.Type = "none"; break;
            }
            return kb;
        }

        /// Repopulate the detail combo for the given type, optionally selecting a
        /// value. window/shortcut/media each get their own option list; other
        /// types leave it empty and insensitive.
        void PopulateDetail(string type, string? select)
        {
            _detail.RemoveAll();
            string[] opts = type.ToLowerInvariant() switch
            {
                "window"   => WindowOps,
                "shortcut" => Protocol.HidKeys.Select(h => h.Name).ToArray(),
                "media"    => Protocol.MediaKeys.Select(m => m.Name).ToArray(),
                _          => Array.Empty<string>(),
            };
            foreach (var o in opts) _detail.AppendText(o);
            _detail.Sensitive = opts.Length > 0;
            _mods.Sensitive = type.Equals("shortcut", StringComparison.OrdinalIgnoreCase);
            if (opts.Length > 0)
            {
                int idx = select is null ? 0 : Array.FindIndex(opts, o => o == select);
                _detail.Active = Math.Max(0, idx);
            }
        }

        static readonly (string Name, int Bit)[] ModBits =
            { ("ctrl", 1), ("shift", 2), ("alt", 4), ("win", 8) };

        static int ParseMods(string text)
        {
            int m = 0;
            foreach (var part in text.Split(new[] { ',', ' ', '+' }, StringSplitOptions.RemoveEmptyEntries))
                foreach (var (name, bit) in ModBits)
                    if (part.Trim().Equals(name, StringComparison.OrdinalIgnoreCase)) m |= bit;
            return m;
        }

        static string ModsToText(int mod) =>
            string.Join(",", ModBits.Where(mb => (mod & mb.Bit) != 0).Select(mb => mb.Name));
    }
}
