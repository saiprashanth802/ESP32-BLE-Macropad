using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

// ImplicitUsings pulls in System.Drawing and System.Windows.Forms project-wide,
// and this is the only WPF-first file in the project — so every type both
// frameworks name is pinned to its WPF meaning here.
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;
using Color = System.Windows.Media.Color;
using ColorConverter = System.Windows.Media.ColorConverter;
using FontFamily = System.Windows.Media.FontFamily;
using TextBox = System.Windows.Controls.TextBox;
using HorizontalAlignment = System.Windows.HorizontalAlignment;
using VerticalAlignment = System.Windows.VerticalAlignment;

namespace MacroPadDeck;

/// Side-by-side rewrite review, in the app's existing Claude design language.
///
/// Side-by-side rather than an inline word diff on purpose: a diff is the right
/// view for a typo fix, but a tone rewrite changes nearly every word and the
/// diff becomes unreadable noise.
///
/// Built in code rather than XAML — it is one window with a fixed layout, and
/// keeping it self-contained avoids another partial class and .xaml pairing.
///
/// Thread contract: WriteFlow calls these members from the thread pool, so
/// every one marshals to the WPF dispatcher.
public sealed class RewritePreviewWindow : IRewritePreview
{
    // Claude palette — same values as EditorWindow.
    static readonly Brush Page   = Hex("#FAF9F5");
    static readonly Brush Card   = Hex("#F0EEE5");
    static readonly Brush Line   = Hex("#E7E3D8");
    static readonly Brush Accent = Hex("#D97757");
    static readonly Brush Ink    = Hex("#141413");
    static readonly Brush Muted  = Hex("#87867F");

    static SolidColorBrush Hex(string h)
    {
        var b = new SolidColorBrush((Color)ColorConverter.ConvertFromString(h)!);
        b.Freeze();
        return b;
    }

    readonly Dispatcher _ui;

    Window? _win;
    TextBox? _out;
    TextBlock? _origText, _hint, _styleTag, _subjectText;
    Border? _subjectRow;
    bool _closing;

    public event Action? Accept;
    public event Action? Cancel;
    public event Action? Again;

    /// Constructed on the UI thread so CurrentDispatcher is the pump the tray
    /// already runs.
    public RewritePreviewWindow() => _ui = Dispatcher.CurrentDispatcher;

    void On(Action a)
    {
        if (_ui.CheckAccess()) a();
        else _ui.BeginInvoke(a);
    }

    public string Text
    {
        get
        {
            if (_ui.CheckAccess()) return _out?.Text ?? "";
            return _ui.Invoke(() => _out?.Text ?? "");
        }
    }

    // ---- IRewritePreview ------------------------------------------------------

    public void Open(string original, string styleLabel) => On(() =>
    {
        Build();
        _origText!.Text = original;
        _out!.Text = "";
        _styleTag!.Text = styleLabel;
        SetHint("Generating…", Muted);
        HideSubject();
        if (!_win!.IsVisible) _win.Show();
        _win.Activate();
    });

    public void Delta(string text) => On(() =>
    {
        if (_out is null) return;
        _out.AppendText(text);
        _out.ScrollToEnd();
    });

    public void Done(string body, string subject) => On(() =>
    {
        if (_out is null) return;
        // Replace rather than keep the streamed text: what arrived on the wire
        // still carries the model's preamble and any reasoning block, and
        // LlmClient.Clean has stripped those from `body`.
        _out.Text = body;
        _out.ScrollToEnd();

        if (subject.Length > 0)
        {
            _subjectText!.Text = subject;
            _subjectRow!.Visibility = Visibility.Visible;
        }
        else HideSubject();

        SetHint("Ctrl+Enter accept  ·  Esc cancel  ·  pad: ACCEPT / REGEN / CANCEL / BACK", Muted);
        _out.Focus();
        _out.CaretIndex = _out.Text.Length;
    });

    public void Error(string message) => On(() =>
    {
        if (_out is null) return;
        _out.Text = "";
        SetHint(message, Accent);
    });

    public void Close() => On(() =>
    {
        if (_win is null) return;
        _closing = true;               // suppress the Closing→Cancel callback
        _win.Hide();
        _closing = false;
    });

    void HideSubject()
    {
        if (_subjectRow is not null) _subjectRow.Visibility = Visibility.Collapsed;
    }

    void SetHint(string text, Brush brush)
    {
        if (_hint is null) return;
        _hint.Text = text;
        _hint.Foreground = brush;
    }

    // ---- layout ---------------------------------------------------------------

    void Build()
    {
        if (_win is not null) return;

        _styleTag = new TextBlock
        {
            Foreground = Accent, FontWeight = FontWeights.SemiBold, FontSize = 12,
            VerticalAlignment = VerticalAlignment.Center,
        };

        _origText = new TextBlock
        {
            Foreground = Muted, TextWrapping = TextWrapping.Wrap, FontSize = 13, LineHeight = 20,
        };

        _out = new TextBox
        {
            Background = Brushes.Transparent, Foreground = Ink, BorderThickness = new Thickness(0),
            TextWrapping = TextWrapping.Wrap, AcceptsReturn = true, FontSize = 13,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            SelectionBrush = Accent,
        };

        _subjectText = new TextBlock
        {
            Foreground = Ink, TextWrapping = TextWrapping.Wrap, FontSize = 13,
            FontWeight = FontWeights.SemiBold,
        };

        // The generated subject can't be pasted into a compose *body*, so it is
        // surfaced here and put on the clipboard instead.
        _subjectRow = new Border
        {
            Background = Card, BorderBrush = Line, BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(6), Padding = new Thickness(12, 8, 12, 8),
            Margin = new Thickness(0, 0, 0, 10), Visibility = Visibility.Collapsed,
            Child = new StackPanel
            {
                Children =
                {
                    new TextBlock { Text = "SUBJECT (copied to clipboard)", Foreground = Muted, FontSize = 10 },
                    _subjectText,
                },
            },
        };

        _hint = new TextBlock { Foreground = Muted, FontSize = 11, VerticalAlignment = VerticalAlignment.Center };

        var grid = new Grid { Margin = new Thickness(0, 0, 0, 10) };
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(14) });
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.Children.Add(Pane("ORIGINAL", new ScrollViewer
        {
            Content = _origText, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        }, 0));
        grid.Children.Add(Pane("REWRITTEN — editable", _out, 2));

        var root = new DockPanel { Margin = new Thickness(16), LastChildFill = true };

        var header = new DockPanel { Margin = new Thickness(0, 0, 0, 10) };
        header.Children.Add(_styleTag);
        DockPanel.SetDock(_styleTag, Dock.Left);
        header.Children.Add(_hint);
        DockPanel.SetDock(header, Dock.Top);
        root.Children.Add(header);

        DockPanel.SetDock(_subjectRow, Dock.Top);
        root.Children.Add(_subjectRow);
        root.Children.Add(grid);

        _win = new Window
        {
            Title = "Rewrite",
            Width = 940, Height = 480, MinWidth = 560, MinHeight = 320,
            Background = Page,
            WindowStartupLocation = WindowStartupLocation.CenterScreen,
            ShowInTaskbar = false,
            Topmost = true,
            FontFamily = new FontFamily("Segoe UI"),
            Content = root,
        };

        _win.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape) { Cancel?.Invoke(); e.Handled = true; }
            // Plain Enter has to stay available for editing a multi-line
            // rewrite, so accept is the chorded form.
            else if (e.Key == Key.Enter &&
                     (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control)
            { Accept?.Invoke(); e.Handled = true; }
            else if (e.Key == Key.R &&
                     (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control)
            { Again?.Invoke(); e.Handled = true; }
        };

        // Hidden and reused rather than destroyed, so the next rewrite doesn't
        // pay window construction again.
        _win.Closing += (_, e) =>
        {
            if (_closing) return;
            e.Cancel = true;
            _win.Hide();
            Cancel?.Invoke();
        };

        // The tray runs a WinForms pump; without keyboard interop a modeless
        // WPF window gets mouse but never keystrokes (textboxes look dead).
        System.Windows.Forms.Integration.ElementHost.EnableModelessKeyboardInterop(_win);
    }

    static UIElement Pane(string caption, UIElement body, int column)
    {
        var stack = new DockPanel();
        var cap = new TextBlock
        {
            Text = caption, Foreground = Muted, FontSize = 10,
            Margin = new Thickness(2, 0, 0, 5),
        };
        DockPanel.SetDock(cap, Dock.Top);
        stack.Children.Add(cap);
        stack.Children.Add(new Border
        {
            Background = Card, BorderBrush = Line, BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8), Padding = new Thickness(12),
            Child = body,
        });

        Grid.SetColumn(stack, column);
        return stack;
    }
}
