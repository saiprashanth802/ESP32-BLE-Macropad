namespace MacroPadDeck;

/// Linux-specific first-run sample bindings for the DECK preset, wired into
/// ProfileStore.DeckSampleKeys before the store is constructed. These are just
/// starting points a user edits — chosen to be widely available or obviously
/// swappable rather than assuming any one desktop.
static class LinuxDefaults
{
    public static List<KeyBinding> DeckSampleKeys() => new()
    {
        new KeyBinding { Type = "focusOrLaunch", Target = "firefox", Label = "Web" },
        new KeyBinding { Type = "focusOrLaunch", Target = "code",    Label = "Code" },
        new KeyBinding { Type = "open", Target = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), Label = "Home" },
        new KeyBinding { Type = "open", Target = "https://github.com", Label = "GitHub" },
        new KeyBinding { Type = "window", Target = "left",        Label = "SnapL" },
        new KeyBinding { Type = "window", Target = "right",       Label = "SnapR" },
        new KeyBinding { Type = "window", Target = "maximize",    Label = "Max" },
        new KeyBinding { Type = "window", Target = "nextMonitor", Label = "Mon>" },
        new KeyBinding(),                                     // K9 = FN on the pad
        new KeyBinding { Type = "run", Target = "xdg-open", Args = "settings://", Label = "Settings" },
        new KeyBinding(),
        new KeyBinding(),
    };
}
