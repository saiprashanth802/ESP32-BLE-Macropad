using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
// WinForms global usings leak in (UseWindowsForms for the tray) — pin the WPF types
using Button = System.Windows.Controls.Button;
using Color = System.Windows.Media.Color;
using ColorConverter = System.Windows.Media.ColorConverter;
using HorizontalAlignment = System.Windows.HorizontalAlignment;

namespace MacroPadDeck;

/// Visual profile editor. Edits a private copy of the config; Save writes
/// profiles.json, which the running ProfileStore hot-reloads and pushes to
/// the pad (labels + colors) — the editor never talks BLE directly.
public partial class EditorWindow : Window
{
    static EditorWindow? _open;
    public static void Open(DeckController deck)
    {
        if (_open is not null) { _open.Activate(); return; }
        _open = new EditorWindow(deck);
        _open.Closed += (_, _) => _open = null;
        _open.Show();
    }

    // Pastels — chosen to read well on the pad's dark screen and in this UI
    static readonly string[] Palette =
    {
        "#A9C7E8", "#A6D9C3", "#F5D7A0", "#CBB3E6",
        "#F2A6A0", "#A6E0DC", "#F7E8A6", "#EFB6CE",
    };

    static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    DeckConfig _cfg = new();
    Profile? _prof;
    int _keyIdx = -1;
    bool _loading;                       // suppress change-handlers during UI fill
    readonly Button[] _keyBtns = new Button[12];

    EditorWindow(DeckController deck)
    {
        InitializeComponent();

        for (int p = 0; p < 8; p++) ProfPreset.Items.Add(p.ToString());

        foreach (string hex in Palette)
        {
            var b = new Button
            {
                Width = 20, Height = 20, Margin = new Thickness(0, 0, 4, 0),
                Background = Brush(hex), Tag = hex, Cursor = System.Windows.Input.Cursors.Hand,
                Template = SwatchTemplate(),
            };
            b.Click += (_, _) => { ProfColor.Text = (string)b.Tag; };
            Swatches.Items.Add(b);
        }

        for (int k = 0; k < 12; k++)
        {
            int idx = k;
            var btn = new Button { Margin = new Thickness(5), Cursor = System.Windows.Input.Cursors.Hand };
            btn.Click += (_, _) => SelectKey(idx);
            _keyBtns[k] = btn;
            KeyGrid.Children.Add(btn);
        }

        deck.StatusChanged += s => Dispatcher.BeginInvoke(() =>
            LinkStatus.Text = $"● {s}");

        LoadFromDisk();
    }

    // ── model ↔ disk ────────────────────────────
    void LoadFromDisk()
    {
        try { _cfg = JsonSerializer.Deserialize<DeckConfig>(File.ReadAllText(ProfileStore.FilePath), JsonOpts) ?? new(); }
        catch { _cfg = new(); }
        FillProfileList(selectIndex: 0);
    }

    void Save_Click(object s, RoutedEventArgs e)
    {
        File.WriteAllText(ProfileStore.FilePath, JsonSerializer.Serialize(_cfg, JsonOpts));
        Hint.Text = $"Saved {DateTime.Now:HH:mm:ss} — pushed to pad.";
    }

    void Revert_Click(object s, RoutedEventArgs e) { LoadFromDisk(); Hint.Text = "Reverted to last saved state."; }

    // ── profiles ────────────────────────────────
    void FillProfileList(int selectIndex)
    {
        ProfileList.Items.Clear();
        foreach (var p in _cfg.Profiles)
        {
            var row = new DockPanel();
            var dot = new Border
            {
                Width = 10, Height = 10, CornerRadius = new CornerRadius(5),
                Background = Brush(string.IsNullOrWhiteSpace(p.Color) ? "#B9B5A9" : p.Color),
                Margin = new Thickness(0, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center,
            };
            DockPanel.SetDock(dot, Dock.Left);
            var preset = new TextBlock
            {
                Text = $"P{p.Preset + 1}", Foreground = Brush("#87867F"),
                FontSize = 11, VerticalAlignment = VerticalAlignment.Center,
            };
            DockPanel.SetDock(preset, Dock.Right);
            row.Children.Add(dot);
            row.Children.Add(preset);
            row.Children.Add(new TextBlock { Text = p.Name.Length > 0 ? p.Name : "(unnamed)", FontWeight = FontWeights.SemiBold });
            ProfileList.Items.Add(row);
        }
        if (_cfg.Profiles.Count > 0)
            ProfileList.SelectedIndex = Math.Clamp(selectIndex, 0, _cfg.Profiles.Count - 1);
        else
            ShowProfile(null);
    }

    void ProfileList_SelectionChanged(object s, SelectionChangedEventArgs e)
    {
        if (ProfileList.SelectedIndex >= 0 && ProfileList.SelectedIndex < _cfg.Profiles.Count)
            ShowProfile(_cfg.Profiles[ProfileList.SelectedIndex]);
    }

    void ShowProfile(Profile? p)
    {
        _prof = p;
        _loading = true;
        ProfName.Text = p?.Name ?? "";
        ProfPreset.SelectedIndex = p?.Preset ?? -1;
        ProfColor.Text = p?.Color ?? "";
        ProfApps.Text = p is null ? "" : string.Join(", ", p.AppMatch);
        _loading = false;
        RefreshKeyGrid();
        SelectKey(p is null ? -1 : Math.Max(_keyIdx, 0));
    }

    void AddProfile_Click(object s, RoutedEventArgs e)
    {
        var used = _cfg.Profiles.Select(p => p.Preset).ToHashSet();
        int preset = Enumerable.Range(0, 8).FirstOrDefault(i => !used.Contains(i), 0);
        var prof = new Profile { Preset = preset, Name = $"PAGE {preset + 1}", Color = Palette[preset % Palette.Length] };
        for (int i = 0; i < 12; i++) prof.Keys.Add(new KeyBinding());
        _cfg.Profiles.Add(prof);
        FillProfileList(_cfg.Profiles.Count - 1);
    }

    void DeleteProfile_Click(object s, RoutedEventArgs e)
    {
        if (_prof is null) return;
        _cfg.Profiles.Remove(_prof);
        FillProfileList(0);
    }

    void Meta_Changed(object s, EventArgs e)
    {
        if (_loading || _prof is null) return;
        _prof.Name = ProfName.Text;
        if (ProfPreset.SelectedIndex >= 0) _prof.Preset = ProfPreset.SelectedIndex;
        _prof.Color = ProfColor.Text;
        _prof.AppMatch = ProfApps.Text.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
        // refresh list row (name/dot) without rebuilding selection
        int keep = ProfileList.SelectedIndex;
        FillProfileList(keep);
    }

    // ── key grid ────────────────────────────────
    void RefreshKeyGrid()
    {
        for (int k = 0; k < 12; k++)
        {
            var b = _prof?.Keys.ElementAtOrDefault(k);
            bool bound = b is not null && b.Type != "none";
            string title = k == 8 ? "K9 · FN" : $"K{k + 1}";
            string label = bound && b!.Label.Length > 0 ? b.Label : (bound ? b!.Type : "—");
            _keyBtns[k].Content = KeyTile(title, label, bound);
            _keyBtns[k].Template = KeyTileTemplate(k == _keyIdx, bound,
                string.IsNullOrWhiteSpace(_prof?.Color) ? "#D97757" : _prof!.Color);
        }
    }

    void SelectKey(int idx)
    {
        _keyIdx = idx;
        RefreshKeyGrid();
        _loading = true;
        if (idx < 0 || _prof is null)
        {
            BindTitle.Text = "Select a key";
            BindType.SelectedIndex = -1;
        }
        else
        {
            while (_prof.Keys.Count < 12) _prof.Keys.Add(new KeyBinding());
            var b = _prof.Keys[idx];
            BindTitle.Text = idx == 8 ? "Key 9 (FN)" : $"Key {idx + 1}";
            BindType.SelectedIndex = TypeToIndex(b.Type);
            BindTarget.Text = b.Type == "window" ? "" : b.Target;
            BindWindowOp.SelectedIndex = WindowOpToIndex(b.Target);
            BindArgs.Text = b.Args;
            BindHidden.IsChecked = b.Hidden;
            BindLabel.Text = b.Label;
        }
        _loading = false;
        UpdateFieldVisibility();
    }

    void BindType_Changed(object s, EventArgs e) { if (!_loading) { Bind_Changed(s, e); } UpdateFieldVisibility(); }

    void Bind_Changed(object s, EventArgs e)
    {
        if (_loading || _prof is null || _keyIdx < 0) return;
        var b = _prof.Keys[_keyIdx];
        b.Type = IndexToType(BindType.SelectedIndex);
        b.Target = b.Type == "window" ? IndexToWindowOp(BindWindowOp.SelectedIndex) : BindTarget.Text;
        b.Args = BindArgs.Text;
        b.Hidden = BindHidden.IsChecked == true;
        b.Label = BindLabel.Text;
        RefreshKeyGrid();
    }

    void Browse_Click(object s, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Filter = "Programs (*.exe)|*.exe|All files (*.*)|*.*",
            Title = "Pick a program",
        };
        if (dlg.ShowDialog() == true) BindTarget.Text = dlg.FileName;
    }

    void UpdateFieldVisibility()
    {
        string t = IndexToType(BindType.SelectedIndex);
        bool win = t == "window";
        bool run = t == "run";
        bool none = t == "none";
        TargetRow.Visibility = win || none ? Visibility.Collapsed : Visibility.Visible;
        TargetLabel.Visibility = none ? Visibility.Collapsed : Visibility.Visible;
        BindWindowOp.Visibility = win ? Visibility.Visible : Visibility.Collapsed;
        ArgsLabel.Visibility = run || t == "focusOrLaunch" ? Visibility.Visible : Visibility.Collapsed;
        BindArgs.Visibility = ArgsLabel.Visibility;
        BindHidden.Visibility = run ? Visibility.Visible : Visibility.Collapsed;
        BrowseBtn.Visibility = t == "focusOrLaunch" || run ? Visibility.Visible : Visibility.Collapsed;
        BindLabel.Visibility = none ? Visibility.Collapsed : Visibility.Visible;
    }

    // ── helpers ─────────────────────────────────
    static int TypeToIndex(string t) => t.ToLowerInvariant() switch
    {
        "focusorlaunch" => 1, "open" => 2, "run" => 3, "window" => 4, _ => 0,
    };
    static string IndexToType(int i) => i switch
    {
        1 => "focusOrLaunch", 2 => "open", 3 => "run", 4 => "window", _ => "none",
    };
    static int WindowOpToIndex(string op) => op.ToLowerInvariant() switch
    {
        "left" => 0, "right" => 1, "maximize" => 2, "minimize" => 3, "nextmonitor" => 4, _ => 0,
    };
    static string IndexToWindowOp(int i) => i switch
    {
        1 => "right", 2 => "maximize", 3 => "minimize", 4 => "nextMonitor", _ => "left",
    };

    static SolidColorBrush Brush(string hex)
    {
        try { return new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex)); }
        catch { return new SolidColorBrush(Colors.Gray); }
    }

    static StackPanel KeyTile(string title, string label, bool bound) => new()
    {
        VerticalAlignment = VerticalAlignment.Center,
        Children =
        {
            new TextBlock { Text = title, FontSize = 10.5, Foreground = Brush("#87867F"), HorizontalAlignment = HorizontalAlignment.Center },
            new TextBlock
            {
                Text = label, FontSize = 13, FontWeight = FontWeights.SemiBold,
                Foreground = Brush(bound ? "#141413" : "#B9B5A9"),
                HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 2, 0, 0),
            },
        },
    };

    static ControlTemplate KeyTileTemplate(bool selected, bool bound, string accent)
    {
        var border = new FrameworkElementFactory(typeof(Border));
        border.SetValue(Border.BackgroundProperty, Brush(bound ? "#FFFFFF" : "#F7F5EE"));
        border.SetValue(Border.CornerRadiusProperty, new CornerRadius(12));
        border.SetValue(Border.BorderThicknessProperty, new Thickness(selected ? 2 : 1));
        border.SetValue(Border.BorderBrushProperty, Brush(selected ? accent : "#DAD5C9"));
        var content = new FrameworkElementFactory(typeof(ContentPresenter));
        content.SetValue(HorizontalAlignmentProperty, HorizontalAlignment.Center);
        content.SetValue(VerticalAlignmentProperty, VerticalAlignment.Center);
        border.AppendChild(content);
        return new ControlTemplate(typeof(Button)) { VisualTree = border };
    }

    static ControlTemplate SwatchTemplate()
    {
        var border = new FrameworkElementFactory(typeof(Border));
        border.SetBinding(Border.BackgroundProperty, new System.Windows.Data.Binding("Background")
        { RelativeSource = new System.Windows.Data.RelativeSource(System.Windows.Data.RelativeSourceMode.TemplatedParent) });
        border.SetValue(Border.CornerRadiusProperty, new CornerRadius(10));
        border.SetValue(Border.BorderThicknessProperty, new Thickness(1));
        border.SetValue(Border.BorderBrushProperty, Brush("#DAD5C9"));
        return new ControlTemplate(typeof(Button)) { VisualTree = border };
    }
}
