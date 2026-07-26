namespace MacroPadDeck;

/// Windows-specific first-run sample bindings for the DECK preset. Wired into
/// ProfileStore.DeckSampleKeys in Program before the store is constructed, so
/// the shared Core store stays platform-neutral.
static class WindowsDefaults
{
    public static List<KeyBinding> DeckSampleKeys() => new()
    {
        new KeyBinding { Type = "focusOrLaunch", Target = "notepad", Label = "Notepad" },
        new KeyBinding { Type = "focusOrLaunch", Target = "calc",    Label = "Calc" },
        new KeyBinding { Type = "open", Target = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), Label = "Home" },
        new KeyBinding { Type = "open", Target = "https://github.com", Label = "GitHub" },
        new KeyBinding { Type = "window", Target = "left",        Label = "SnapL" },
        new KeyBinding { Type = "window", Target = "right",       Label = "SnapR" },
        new KeyBinding { Type = "window", Target = "maximize",    Label = "Max" },
        new KeyBinding { Type = "window", Target = "nextMonitor", Label = "Mon>" },
        new KeyBinding(),                                     // K9 = FN on the pad
        new KeyBinding { Type = "run", Target = "powershell", Args = "-NoProfile -Command \"Start-Process ms-settings:\"", Label = "Settings" },
        new KeyBinding(),
        new KeyBinding(),
    };
}
