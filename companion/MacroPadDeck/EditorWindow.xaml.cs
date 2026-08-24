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
        // The tray runs a WinForms pump; without keyboard interop a modeless
        // WPF window gets mouse but never keystrokes (textboxes look dead).
        System.Windows.Forms.Integration.ElementHost.EnableModelessKeyboardInterop(_open);
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
    readonly DeckController _deck;
    List<PadAction> _padActions = new();

    EditorWindow(DeckController deck)
    {
        InitializeComponent();
        _deck = deck;

        for (int p = 0; p < 8; p++) ProfPreset.Items.Add(p.ToString());
        foreach (var (name, _) in Protocol.HidKeys) BindKeyCombo.Items.Add(name);
        foreach (var (name, _) in Protocol.MediaKeys) BindMedia.Items.Add(name);

        // The builtin list comes off the pad, so it can only be filled once the
        // link is up. Fire and forget — the picker fills in when it arrives.
        _ = LoadPadActions();

        // Eye swatches: robotic blue first (the default face), then the pastels
        foreach (string hex in new[] { "#3ABEFF" }.Concat(Palette.Take(5)))
        {
            var b = new Button
            {
                Width = 20, Height = 20, Margin = new Thickness(0, 0, 4, 0),
                Background = Brush(hex), Tag = hex, Cursor = System.Windows.Input.Cursors.Hand,
                Template = SwatchTemplate(),
            };
            b.Click += (_, _) => { EyeColorBox.Text = (string)b.Tag; };
            EyeSwatches.Items.Add(b);
        }

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
            var btn = new Button { Margin = new Thickness(4), Cursor = System.Windows.Input.Cursors.Hand };
            btn.Click += (_, _) => SelectKey(idx);
            _keyBtns[k] = btn;
            KeyGrid.Children.Add(btn);
        }

        deck.StatusChanged += s => Dispatcher.BeginInvoke(() =>
        {
            LinkStatus.Text = $"● {s}";
            bool up = s.Contains("online") || s.Contains("Connected") ||
                      s.Contains("written") || s.Contains("reloaded");
            // Darkened from #4C9A6B / #B0553E, which sat near 3:1 on the card at 11px.
            LinkStatus.Foreground = Brush(up ? "#3D7F57" : "#9E4632");
        });

        LoadFromDisk();
    }

    // ── model ↔ disk ────────────────────────────
    void LoadFromDisk()
    {
        try { _cfg = JsonSerializer.Deserialize<DeckConfig>(File.ReadAllText(ProfileStore.FilePath), JsonOpts) ?? new(); }
        catch { _cfg = new(); }
        _loading = true;
        EyeColorBox.Text = _cfg.EyeColor;
        _loading = false;
        FillProfileList(selectIndex: 0);
    }

    void EyeColor_Changed(object s, EventArgs e)
    {
        if (_loading) return;
        _cfg.EyeColor = EyeColorBox.Text;
        _deck.PushEyesLive(_cfg.EyeColor);     // live preview on the pad
    }

    void EyePreset_Click(object s, RoutedEventArgs e) { EyeColorBox.Text = "preset"; }

    void Save_Click(object s, RoutedEventArgs e)
    {
        File.WriteAllText(ProfileStore.FilePath, JsonSerializer.Serialize(_cfg, JsonOpts));
        _ = _deck.ApplyPadConfig(_cfg);    // rewrite app-managed keys on the pad
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
                Text = $"P{p.Preset + 1}", Foreground = Brush("#6B6A63"),
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
        if (_prof.Color != ProfColor.Text)
        {
            _prof.Color = ProfColor.Text;
            _deck.PushColorLive(_prof.Preset, _prof.Color);   // live preview
        }
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
            // Unbound keys still show their label (grey) — presets 0-6 carry
            // the pad's firmware shortcuts, which the app displays but doesn't own
            string label = b is not null && b.Label.Length > 0 ? b.Label : (bound ? b!.Type : "—");
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
            ModCtrl.IsChecked  = (b.Mod & 1) != 0;
            ModShift.IsChecked = (b.Mod & 2) != 0;
            ModAlt.IsChecked   = (b.Mod & 4) != 0;
            ModWin.IsChecked   = (b.Mod & 8) != 0;
            BindKeyCombo.SelectedIndex = Array.FindIndex(Protocol.HidKeys, h => h.Name == b.Key);
            BindMedia.SelectedIndex = Array.FindIndex(Protocol.MediaKeys, m => m.Name == b.Media);
            // Match on id, not label — a renamed action keeps working
            BindPadAction.SelectedIndex = _padActions.FindIndex(a => a.Id == b.ActionId);
        }
        _loading = false;
        UpdateFieldVisibility();
    }

    /// Pull the pad's builtin action library and fill the picker. Cached after
    /// the first success, so reopening the editor doesn't re-fetch.
    async Task LoadPadActions()
    {
        var src = _deck.Actions;
        if (src is null) { PadActionHint.Text = "Pad action list unavailable."; return; }

        if (!src.HasData)
        {
            PadActionHint.Text = "Loading actions from pad…";
            bool ok = await src.Fetch();
            if (!ok)
            {
                // Almost always means the pad is on firmware without
                // HCMD_ACTIONS, or the link dropped mid-fetch.
                PadActionHint.Text = "Couldn't read the action list — is the pad connected "
                                   + "and on current firmware?";
                return;
            }
        }

        _padActions = src.Items.ToList();
        BindPadAction.Items.Clear();
        foreach (var a in _padActions) BindPadAction.Items.Add(a.Label);
        PadActionHint.Text = $"{_padActions.Count} actions from the pad — runs on the pad itself, "
                           + "no host involvement.";

        // A key may already be bound to one; re-select now the list exists
        if (_prof is not null && _keyIdx >= 0 && _keyIdx < _prof.Keys.Count)
        {
            _loading = true;
            BindPadAction.SelectedIndex = _padActions.FindIndex(a => a.Id == _prof.Keys[_keyIdx].ActionId);
            _loading = false;
        }
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
        b.Mod = (ModCtrl.IsChecked == true ? 1 : 0) | (ModShift.IsChecked == true ? 2 : 0)
              | (ModAlt.IsChecked == true ? 4 : 0) | (ModWin.IsChecked == true ? 8 : 0);
        b.Key = BindKeyCombo.SelectedIndex >= 0 ? Protocol.HidKeys[BindKeyCombo.SelectedIndex].Name : "";
        b.Media = BindMedia.SelectedIndex >= 0 ? Protocol.MediaKeys[BindMedia.SelectedIndex].Name : "";
        if (BindPadAction.SelectedIndex >= 0 && BindPadAction.SelectedIndex < _padActions.Count)
            b.ActionId = _padActions[BindPadAction.SelectedIndex].Id;
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
        bool shortcut = t == "shortcut";
        bool media = t == "media";
        bool text = t == "text";
        bool needsTarget = t is "focusOrLaunch" or "open" or "run" or "text";
        // favorite takes no parameters — it acts on whatever is playing
        TargetRow.Visibility = needsTarget ? Visibility.Visible : Visibility.Collapsed;
        TargetLabel.Visibility = needsTarget ? Visibility.Visible : Visibility.Collapsed;
        TargetLabel.Text = text ? "TEXT TO TYPE (≤ 23 chars)" : "TARGET";
        BindWindowOp.Visibility = win ? Visibility.Visible : Visibility.Collapsed;
        ShortcutPanel.Visibility = shortcut ? Visibility.Visible : Visibility.Collapsed;
        BindMedia.Visibility = media ? Visibility.Visible : Visibility.Collapsed;
        PadActionPanel.Visibility = t == "padAction" ? Visibility.Visible : Visibility.Collapsed;
        // write takes no parameters at all — only the pad label applies.
        WriteHint.Visibility = t == "write" ? Visibility.Visible : Visibility.Collapsed;
        ArgsLabel.Visibility = run || t == "focusOrLaunch" ? Visibility.Visible : Visibility.Collapsed;
        BindArgs.Visibility = ArgsLabel.Visibility;
        BindHidden.Visibility = run ? Visibility.Visible : Visibility.Collapsed;
        BrowseBtn.Visibility = t == "focusOrLaunch" || run ? Visibility.Visible : Visibility.Collapsed;
        BindLabel.Visibility = none ? Visibility.Collapsed : Visibility.Visible;
    }

    // ── helpers ─────────────────────────────────
    // Indices are positions in BindType's ComboBoxItem list — append only.
    static int TypeToIndex(string t) => t.ToLowerInvariant() switch
    {
        "focusorlaunch" => 1, "open" => 2, "run" => 3, "window" => 4,
        "shortcut" => 5, "media" => 6, "text" => 7, "favorite" => 8,
        "padaction" => 9, "write" => 10, _ => 0,
    };
    static string IndexToType(int i) => i switch
    {
        1 => "focusOrLaunch", 2 => "open", 3 => "run", 4 => "window",
        5 => "shortcut", 6 => "media", 7 => "text", 8 => "favorite",
        9 => "padAction", 10 => "write", _ => "none",
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
            new TextBlock { Text = title, FontSize = 11, Foreground = Brush("#6B6A63"), HorizontalAlignment = HorizontalAlignment.Center },
            new TextBlock
            {
                Text = label, FontSize = 13, FontWeight = FontWeights.SemiBold,
                // #B9B5A9 put the unbound label near 2:1 against the tile. #7D7C74 keeps
                // it a clear tier below ink without being unreadable.
                Foreground = Brush(bound ? "#141413" : "#7D7C74"),
                HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 2, 0, 0),
            },
        },
    };

    /// White tile washed with ~14% of the profile color — bound keys visibly
    /// belong to their profile without shouting.
    static SolidColorBrush Tint(string hex)
    {
        try
        {
            var c = (Color)ColorConverter.ConvertFromString(hex);
            return new SolidColorBrush(Color.FromRgb(
                (byte)(255 - (255 - c.R) * 0.14), (byte)(255 - (255 - c.G) * 0.14),
                (byte)(255 - (255 - c.B) * 0.14)));
        }
        catch { return new SolidColorBrush(Colors.White); }
    }

    static ControlTemplate KeyTileTemplate(bool selected, bool bound, string accent)
    {
        var border = new FrameworkElementFactory(typeof(Border));
        border.SetValue(Border.BackgroundProperty, bound ? Tint(accent) : Brush("#F1EFE6"));
        border.SetValue(Border.CornerRadiusProperty, new CornerRadius(12));
        // Thickness is constant so selecting never nudges the grid by a pixel, and the
        // ring is ink rather than the profile colour: selection used to be drawn in the
        // profile's own hue, so a pale profile (#F7E8A6) was near-invisible against the
        // tile. Ink is also now the only border in the grid, so "outlined" reads as
        // "selected" and nothing else.
        border.SetValue(Border.BorderThicknessProperty, new Thickness(2));
        border.SetValue(Border.BorderBrushProperty,
            selected ? Brush("#141413") : (System.Windows.Media.Brush)System.Windows.Media.Brushes.Transparent);
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
