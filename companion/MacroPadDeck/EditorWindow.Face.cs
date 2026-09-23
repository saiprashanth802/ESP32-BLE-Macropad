using System.Windows;
using System.Windows.Controls;
using Button = System.Windows.Controls.Button;
using ComboBox = System.Windows.Controls.ComboBox;
using TextBox = System.Windows.Controls.TextBox;
using HorizontalAlignment = System.Windows.HorizontalAlignment;

namespace MacroPadDeck;

/// The editor's Face view: face.json as a table (event → emote, chance,
/// cooldown) plus the dance sliders. Works on a copy; Save writes face.json
/// and persists it on the pad. Dance sliders and ▶ buttons act on the pad
/// live without saving.
public partial class EditorWindow
{
    FaceConfig _face = FaceConfig.Defaults();
    bool _faceBuilt;

    static readonly Dictionary<string, string> PadEventNames = new()
    {
        ["boot"] = "Pad boots",              ["connect"] = "Host connects",
        ["disconnect"] = "Host disconnects", ["typing"] = "Typing burst",
        ["idle"] = "Idle a minute+",         ["wake"] = "Woken by a key",
        ["preset"] = "Preset picked on pad", ["slot"] = "Host slot switch",
        ["track"] = "New track",             ["favourite"] = "Track favourited",
        ["paused"] = "Music paused",         ["flair"] = "Dance flair",
    };

    static string HostEventName(string key) => key switch
    {
        "drop" => "Music drop (PC)",
        "late" => "Late night starts (PC)",
        _ when key.StartsWith("app:") => $"Switch to a {key[4..]} app (PC)",
        _ => key,
    };

    void TabKeys_Click(object s, RoutedEventArgs e) => ShowFaceView(false);
    void TabFace_Click(object s, RoutedEventArgs e) => ShowFaceView(true);

    bool FaceVisible => FaceView.Visibility == Visibility.Visible;

    void ShowFaceView(bool face)
    {
        if (face && !_faceBuilt) { LoadFace(); _faceBuilt = true; }
        FaceView.Visibility = face ? Visibility.Visible : Visibility.Collapsed;
        KeysView.Visibility = face ? Visibility.Collapsed : Visibility.Visible;
        TabFace.Foreground = (System.Windows.Media.Brush)FindResource(face ? "EmberFlat" : "Muted");
        TabKeys.Foreground = (System.Windows.Media.Brush)FindResource(face ? "Muted" : "EmberFlat");
        Hint.Text = face ? "Saving writes face.json and stores the table on the pad."
                         : "Saving writes profiles.json — labels and colours push to the pad instantly.";
        if (face) RefreshFacePadState();
    }

    void LoadFace()
    {
        var dir = _deck.Face;
        _face = dir is null ? FaceConfig.Defaults() : dir.Store.Copy();
        _loading = true;
        DanceLevel.Value = _face.DanceLevel;
        FlairBars.Value = _face.FlairBars;
        _loading = false;
        UpdateDanceLabels();
        BuildFaceRules();
        BuildEmoteButtons();
        if (dir is not null && !_faceHooked)
        {
            _faceHooked = true;
            dir.PadChanged += () => Dispatcher.BeginInvoke(RefreshFacePadState);
        }
    }
    bool _faceHooked;

    void BuildFaceRules()
    {
        FaceRules.Children.Clear();
        AddRuleHeader("ON THE PAD");
        foreach (var t in Protocol.PadTriggers)
            if (_face.Pad.TryGetValue(t, out var r))
                FaceRules.Children.Add(RuleRow(PadEventNames.GetValueOrDefault(t, t), r, allowRandom: t == "flair", maxCd: 255));
        AddRuleHeader("FROM THE PC");
        foreach (var (k, r) in _face.Host.OrderBy(kv => kv.Key.StartsWith("app:") ? 1 : 0).ThenBy(kv => kv.Key))
            FaceRules.Children.Add(RuleRow(HostEventName(k), r, allowRandom: false, maxCd: 86400));
    }

    void AddRuleHeader(string text) =>
        FaceRules.Children.Add(new TextBlock
        {
            Text = text, Style = (Style)FindResource("SectionLabel"), Margin = new Thickness(0, 12, 0, 4),
        });

    FrameworkElement RuleRow(string label, EmoteRule r, bool allowRandom, int maxCd)
    {
        var g = new Grid { Margin = new Thickness(0, 3, 0, 3) };
        foreach (var w in new[] { 170.0, 120, -1, 70, 44 })
            g.ColumnDefinitions.Add(new ColumnDefinition
            { Width = w < 0 ? new GridLength(1, GridUnitType.Star) : new GridLength(w) });

        var name = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center,
                                   Foreground = (System.Windows.Media.Brush)FindResource("Bone") };

        var emote = new ComboBox { Margin = new Thickness(0, 0, 10, 0) };
        if (allowRandom) emote.Items.Add(Protocol.EmoteRandom);
        foreach (var e in Protocol.Emotes) emote.Items.Add(e);
        emote.SelectedItem = emote.Items.Cast<string>()
            .FirstOrDefault(x => x.Equals(r.Emote, StringComparison.OrdinalIgnoreCase)) ?? "neutral";
        emote.SelectionChanged += (_, _) => { if (emote.SelectedItem is string s) r.Emote = s; };

        var chanceOut = new TextBlock { Width = 40, Margin = new Thickness(10, 0, 0, 0), Style = (Style)FindResource("MonoMuted"),
                                        VerticalAlignment = VerticalAlignment.Center };
        var chance = new Slider { Minimum = 0, Maximum = 100, TickFrequency = 5, IsSnapToTickEnabled = true,
                                  Value = r.Chance, VerticalAlignment = VerticalAlignment.Center };
        void ShowChance() { chanceOut.Text = chance.Value == 0 ? "off" : $"{chance.Value:0}%";
                            name.Opacity = chance.Value == 0 ? 0.45 : 1; }
        chance.ValueChanged += (_, _) => { r.Chance = (int)chance.Value; ShowChance(); };
        ShowChance();
        var chanceRow = new DockPanel { Margin = new Thickness(0, 0, 10, 0) };
        DockPanel.SetDock(chanceOut, Dock.Right);
        chanceRow.Children.Add(chanceOut); chanceRow.Children.Add(chance);

        var cd = new TextBox { Text = r.CooldownS.ToString(), ToolTip = "Cooldown in seconds",
                               Margin = new Thickness(0, 0, 6, 0) };
        cd.TextChanged += (_, _) => { if (int.TryParse(cd.Text, out int v)) r.CooldownS = Math.Clamp(v, 0, maxCd); };

        var play = new Button { Content = "▶", Style = (Style)FindResource("GhostBtn"), Padding = new Thickness(8, 3, 8, 3),
                                ToolTip = "Play this emote on the pad", HorizontalAlignment = HorizontalAlignment.Left };
        play.Click += async (_, _) => await PlayOnPad(r.Emote);

        Grid.SetColumn(name, 0); Grid.SetColumn(emote, 1); Grid.SetColumn(chanceRow, 2);
        Grid.SetColumn(cd, 3); Grid.SetColumn(play, 4);
        g.Children.Add(name); g.Children.Add(emote); g.Children.Add(chanceRow); g.Children.Add(cd); g.Children.Add(play);
        return g;
    }

    void BuildEmoteButtons()
    {
        EmoteButtons.Children.Clear();
        foreach (var e in Protocol.Emotes)
        {
            var b = new Button { Content = e, Style = (Style)FindResource("GhostBtn"),
                                 Padding = new Thickness(9, 4, 9, 4), Margin = new Thickness(0, 0, 6, 6) };
            b.Click += async (_, _) => await PlayOnPad(e);
            EmoteButtons.Children.Add(b);
        }
    }

    async Task PlayOnPad(string emote)
    {
        var dir = _deck.Face;
        bool ok = dir is not null && await dir.Play(emote);
        Hint.Text = ok ? $"Playing {emote} on the pad." : "Pad not connected, or its firmware has no face v3.";
    }

    void Dance_Changed(object s, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_loading || !_faceBuilt) return;
        _face.DanceLevel = (int)DanceLevel.Value;
        _face.FlairBars = (int)FlairBars.Value;
        UpdateDanceLabels();
        _ = _deck.Face?.PreviewDance(_face.DanceLevel, _face.FlairBars);
    }

    void UpdateDanceLabels()
    {
        int lv = (int)DanceLevel.Value, fb = (int)FlairBars.Value;
        DanceLevelOut.Text = lv == 0 ? "off" : lv < 35 ? $"{lv} · nod" : lv < 70 ? $"{lv} · groove" : $"{lv} · party";
        FlairOut.Text = fb == 0 ? "never" : $"{fb} bars";
    }

    void RefreshFacePadState()
    {
        var dir = _deck.Face;
        FacePadState.Text = dir is null ? "Face engine not running."
            : dir.PadSupported ? "Pad runs face v3 — changes apply live."
            : dir.PadCaps < 0 ? "Pad not connected — settings save now and reach the pad on connect."
            : "The pad's firmware predates face v3 — flash it to use these.";
    }

    async Task SaveFace()
    {
        var dir = _deck.Face;
        if (dir is null) return;
        dir.Store.Save(_face);                      // stores a copy; we keep editing ours
        bool ok = await dir.PushConfig(persist: true);
        Hint.Text = ok ? $"Saved {DateTime.Now:HH:mm:ss} — face stored on the pad."
                       : $"Saved {DateTime.Now:HH:mm:ss} — reaches the pad on its next connect.";
    }
}
