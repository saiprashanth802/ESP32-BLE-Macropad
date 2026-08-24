using Gtk;
// Gtk.Action (the deprecated GtkAction widget) collides with System.Action, and
// IRewritePreview's events are the latter. Pin it for the whole file.
using Action = System.Action;

namespace MacroPadDeck;

/// The rewrite review surface on Linux — GTK counterpart of the WPF
/// RewritePreviewWindow. Same contract, same palette, one deliberate
/// behavioural difference.
///
/// **The window does not take focus.** WriteFlow's premise is that the pad is
/// the input device and the monitor is only the display, so nothing here needs
/// the keyboard. On Windows that is a nicety; on Wayland it is what makes the
/// feature work at all, because a compositor gives no way to hand focus back to
/// the app the selection came from. Keeping focus where it already is means the
/// paste lands correctly without ever needing to move it. See LinuxTextCapture.
///
/// Under X11 focus *can* be restored, so the window behaves like the Windows
/// one: focusable, editable, Ctrl+Enter / Esc bound.
///
/// Thread contract: WriteFlow calls these members from the thread pool, so
/// every one marshals onto the GTK main loop.
public sealed class GtkRewritePreview : IRewritePreview
{
    // Claude palette — same values as the WPF preview. Muted is the corrected
    // #6B6A63 rather than the original #87867F, which was 3.15:1 on Card and
    // failed WCAG AA.
    const string Page = "#FAF9F5", Card = "#F0EEE5", Line = "#E7E3D8";
    const string Accent = "#D97757", Ink = "#141413", Muted = "#6B6A63";

    readonly bool _wayland =
        string.Equals(Environment.GetEnvironmentVariable("XDG_SESSION_TYPE"), "wayland",
                      StringComparison.OrdinalIgnoreCase);

    Window? _win;
    TextView? _out;
    Label? _origText, _hint, _styleTag, _subjectText;
    Widget? _subjectRow;

    /// Set if the window ever receives focus. LinuxTextCapture refuses to paste
    /// while this is true — under Wayland it is the only evidence available that
    /// the keyboard has left the app the selection came from.
    volatile bool _tookFocus;

    public event Action? Accept;
    public event Action? Cancel;
    public event Action? Again;

    /// Asked by LinuxTextCapture immediately before pasting.
    public bool FocusIsSafe => !_tookFocus;

    static void On(Action a) => Application.Invoke((_, _) => a());

    /// Read across threads by WriteFlow.DoAccept. GTK has no Dispatcher.Invoke
    /// equivalent that returns a value, so the buffer text is mirrored here on
    /// every change and served from the mirror.
    volatile string _text = "";
    public string Text => _text;

    // ── IRewritePreview ─────────────────────────────────────────────────────────

    public void Open(string original, string styleLabel) => On(() =>
    {
        Build();
        _origText!.Text = Ellipsis(original, 600);
        SetOut("");
        _styleTag!.Text = styleLabel;
        SetHint("Generating…", Muted);
        _subjectRow!.Visible = false;
        _tookFocus = false;

        _win!.ShowAll();
        _subjectRow.Visible = false;      // ShowAll would otherwise reveal it
        // Present() would demand focus; on Wayland that is exactly what must not
        // happen, so the window is only mapped and left where the WM puts it.
        if (!_wayland) _win.Present();
    });

    public void Delta(string text) => On(() =>
    {
        if (_out is null) return;
        var end = _out.Buffer.EndIter;
        _out.Buffer.Insert(ref end, text);
        _text = _out.Buffer.Text;
        ScrollToEnd();
    });

    public void Done(string body, string subject) => On(() =>
    {
        // Replace rather than keep the streamed text: what arrived on the wire
        // still carries the model's preamble and any reasoning block, and
        // LlmClient.Clean has stripped those from `body`.
        SetOut(body);
        ScrollToEnd();

        if (subject.Length > 0)
        {
            _subjectText!.Text = subject;
            _subjectRow!.Visible = true;
        }
        else _subjectRow!.Visible = false;

        SetHint(_wayland
            ? "pad: ACCEPT / REGEN / CANCEL / BACK  ·  clicking this window cancels the paste"
            : "Ctrl+Enter accept  ·  Esc cancel  ·  pad: ACCEPT / REGEN / CANCEL / BACK",
            Muted);
    });

    public void Error(string message) => On(() =>
    {
        SetOut("");
        SetHint(message, Accent);
    });

    public void Close() => On(() => _win?.Hide());

    // ── layout ──────────────────────────────────────────────────────────────────

    void SetOut(string s)
    {
        if (_out is null) return;
        _out.Buffer.Text = s;
        _text = s;
    }

    void ScrollToEnd()
    {
        if (_out is null) return;
        var mark = _out.Buffer.CreateMark(null, _out.Buffer.EndIter, false);
        _out.ScrollToMark(mark, 0, false, 0, 0);
        _out.Buffer.DeleteMark(mark);
    }

    void SetHint(string text, string color)
    {
        if (_hint is null) return;
        _hint.Text = text;
        Style(_hint, $"color:{color};font-size:11px;");
    }

    static string Ellipsis(string s, int max) =>
        s.Length <= max ? s : s[..max] + "…";

    void Build()
    {
        if (_win is not null) return;

        _win = new Window("MacroPad — rewrite")
        {
            SkipTaskbarHint = true,
            SkipPagerHint = true,
            TypeHint = Gdk.WindowTypeHint.Utility,
            // The one line that makes the Wayland paste-back work: the WM never
            // gives this window the keyboard, so the target app keeps it.
            AcceptFocus = !_wayland,
            Resizable = true,
        };
        _win.SetDefaultSize(560, 520);
        _win.SetPosition(WindowPosition.CenterAlways);

        // Hiding rather than destroying — WriteFlow reuses one preview for the
        // life of the process, and Close() is called on every teardown.
        _win.DeleteEvent += (_, a) => { a.RetVal = true; _win!.Hide(); Cancel?.Invoke(); };
        _win.FocusInEvent += (_, _) => _tookFocus = true;

        Style(_win, $"background-color:{Page};");

        var root = new Box(Orientation.Vertical, 10) { BorderWidth = 16 };

        // ── header: style tag ────────────────────────────────────────────────
        _styleTag = new Label("") { Halign = Align.Start };
        Style(_styleTag, $"color:{Accent};font-weight:600;font-size:12px;");
        root.PackStart(_styleTag, false, false, 0);

        // ── original (dim, capped) ───────────────────────────────────────────
        _origText = new Label("") { Halign = Align.Start, Xalign = 0, Wrap = true, LineWrapMode = Pango.WrapMode.WordChar };
        Style(_origText, $"color:{Muted};font-size:12px;");
        var origFrame = Framed(_origText);
        var origScroll = new ScrolledWindow { HeightRequest = 110 };
        origScroll.SetPolicy(PolicyType.Never, PolicyType.Automatic);
        origScroll.Add(origFrame);
        root.PackStart(origScroll, false, false, 0);

        // ── generated subject, when the style produced one ───────────────────
        // A subject can't be pasted into a compose *body*, so it is surfaced
        // here and put on the clipboard instead.
        _subjectText = new Label("") { Halign = Align.Start, Xalign = 0, Wrap = true };
        Style(_subjectText, $"color:{Ink};font-size:12px;font-weight:600;");
        var subjBox = new Box(Orientation.Vertical, 4);
        var subjCap = new Label("SUBJECT — on the clipboard") { Halign = Align.Start, Xalign = 0 };
        Style(subjCap, $"color:{Muted};font-size:10px;letter-spacing:1px;");
        subjBox.PackStart(subjCap, false, false, 0);
        subjBox.PackStart(_subjectText, false, false, 0);
        _subjectRow = Framed(subjBox);
        root.PackStart(_subjectRow, false, false, 0);

        // ── the rewrite itself ───────────────────────────────────────────────
        _out = new TextView
        {
            WrapMode = WrapMode.WordChar,
            // Editing needs focus, which Wayland can't grant without breaking
            // the paste target. The pad is the control surface there.
            Editable = !_wayland,
            CursorVisible = !_wayland,
            LeftMargin = 10, RightMargin = 10, TopMargin = 8, BottomMargin = 8,
        };
        Style(_out, $"background-color:{Card};color:{Ink};font-size:12px;");
        _out.Buffer.Changed += (_, _) => _text = _out.Buffer.Text;

        var outScroll = new ScrolledWindow();
        outScroll.SetPolicy(PolicyType.Never, PolicyType.Automatic);
        outScroll.Add(_out);
        root.PackStart(Framed(outScroll), true, true, 0);

        // ── hint + buttons ───────────────────────────────────────────────────
        _hint = new Label("") { Halign = Align.Start, Xalign = 0, Wrap = true };
        root.PackStart(_hint, false, false, 0);

        var buttons = new Box(Orientation.Horizontal, 8) { Halign = Align.End };
        buttons.PackStart(MakeButton("Cancel", () => Cancel?.Invoke()), false, false, 0);
        buttons.PackStart(MakeButton("Regenerate", () => Again?.Invoke()), false, false, 0);
        var accept = MakeButton("Accept", () => Accept?.Invoke());
        Style(accept, $"background-image:none;background-color:{Accent};color:#FFFFFF;font-weight:600;");
        buttons.PackStart(accept, false, false, 0);
        root.PackStart(buttons, false, false, 0);

        _win.Add(root);

        // Only reachable under X11, where the window is focusable at all.
        _win.KeyPressEvent += (_, a) =>
        {
            bool ctrl = (a.Event.State & Gdk.ModifierType.ControlMask) != 0;
            if (a.Event.Key is Gdk.Key.Escape) Cancel?.Invoke();
            else if (ctrl && a.Event.Key is Gdk.Key.Return or Gdk.Key.KP_Enter) Accept?.Invoke();
        };
    }

    Widget Framed(Widget child)
    {
        var f = new Frame { ShadowType = ShadowType.None };
        f.Add(child);
        Style(f, $"background-color:{Card};border:1px solid {Line};border-radius:6px;padding:10px;");
        return f;
    }

    static Button MakeButton(string label, Action onClick)
    {
        var b = new Button(label) { CanFocus = false };
        b.Clicked += (_, _) => onClick();
        return b;
    }

    /// GtkSharp has no inline-style property, so each widget gets its own CSS
    /// provider. Cheap at this scale (a dozen widgets, built once) and far more
    /// readable than a single stylesheet keyed on widget names.
    static void Style(Widget w, string css)
    {
        var p = new CssProvider();
        p.LoadFromData($"* {{ {css} }}");
        w.StyleContext.AddProvider(p, StyleProviderPriority.Application);
    }
}
