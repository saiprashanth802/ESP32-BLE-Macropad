using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MacroPadDeck;

public sealed class KeyBinding
{
    /// none | focusOrLaunch | open | run | window | shortcut | media | text
    /// App actions (focusOrLaunch/open/run/window) make the pad key type
    /// "host"; shortcut/media/text rewrite the pad key itself over BLE.
    public string Type { get; set; } = "none";
    /// focusOrLaunch: exe path (or bare process name); open: url/file/folder;
    /// run: command line; window: left|right|maximize|minimize|nextMonitor;
    /// text: the snippet to type
    public string Target { get; set; } = "";
    public string Args { get; set; } = "";
    public bool Hidden { get; set; } = true;       // run: no console window
    public string Label { get; set; } = "";        // pushed to the pad (≤8 chars)
    public int Mod { get; set; }                   // shortcut: Ctrl=1 Shift=2 Alt=4 Win=8
    public string Key { get; set; } = "";          // shortcut: name from Protocol.HidKeys
    public string Media { get; set; } = "";        // media: name from Protocol.MediaKeys
}

public sealed class Profile
{
    public int Preset { get; set; }                // pad preset index 0..7 this maps to
    public string Name { get; set; } = "";
    /// "#RRGGBB" — pushed to the pad as this preset's accent + eye color
    public string Color { get; set; } = "";
    /// exe names (no .exe) that auto-activate this profile when focused
    public List<string> AppMatch { get; set; } = new();
    public List<KeyBinding> Keys { get; set; } = new();   // up to 12, index = key idx
}

public sealed class DeckConfig
{
    public string DeviceAddress { get; set; } = "841FE82B334A";
    public int StickySeconds { get; set; } = 30;
    public int DefaultPreset { get; set; } = -1;   // fall back when no AppMatch hits (-1 = stay)
    /// "#RRGGBB", or "preset" to follow the active preset's accent color
    public string EyeColor { get; set; } = "#3ABEFF";   // robotic blue
    /// Stream Windows now-playing (title + timeline) to the pad's face screen
    public bool NowPlaying { get; set; } = true;
    /// Feishin Remote fallback — Feishin doesn't publish to Windows media
    /// sessions, so it's polled directly. Fill in the credentials you set in
    /// Feishin → Settings → Remote; leave the URL blank to disable.
    public string FeishinUrl { get; set; } = "http://127.0.0.1:4333";
    public string FeishinUser { get; set; } = "";
    public string FeishinPassword { get; set; } = "";
    public List<Profile> Profiles { get; set; } = new();
}

/// Owns %APPDATA%\MacroPadDeck\profiles.json. Watches it and hot-reloads,
/// so editing the file in any editor reconfigures the running app.
public sealed class ProfileStore : IDisposable
{
    public static readonly string Dir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MacroPadDeck");
    public static readonly string FilePath = Path.Combine(Dir, "profiles.json");

    static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingDefault,
        AllowTrailingCommas = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    readonly FileSystemWatcher _fsw;
    public DeckConfig Config { get; private set; } = new();
    public event Action? Reloaded;

    public ProfileStore()
    {
        Directory.CreateDirectory(Dir);
        if (!File.Exists(FilePath)) WriteDefault();
        Load();
        _fsw = new FileSystemWatcher(Dir, "profiles.json") { EnableRaisingEvents = true };
        _fsw.Changed += (_, _) => DebouncedReload();
        _fsw.Created += (_, _) => DebouncedReload();
    }

    System.Threading.Timer? _debounce;
    void DebouncedReload() =>
        (_debounce ??= new System.Threading.Timer(_ => { Load(); Reloaded?.Invoke(); }))
            .Change(400, Timeout.Infinite);

    void Load()
    {
        try
        {
            using var fs = new FileStream(FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            Config = JsonSerializer.Deserialize<DeckConfig>(fs, JsonOpts) ?? new DeckConfig();
        }
        catch { /* mid-save or bad JSON — keep the last good config */ }
    }

    public Profile? ForPreset(int preset) => Config.Profiles.FirstOrDefault(p => p.Preset == preset);

    public Profile? ForExe(string exe) => Config.Profiles.FirstOrDefault(
        p => p.AppMatch.Any(m => m.Equals(exe, StringComparison.OrdinalIgnoreCase)));

    void WriteDefault()
    {
        var cfg = new DeckConfig();
        // One profile per pad preset, pastel-coded — names mirror the firmware
        string[] names  = { "ONSHAPE", "KICAD", "MUSIC", "LTSPICE", "GAMING", "SYS", "DEV" };
        string[] pastel = { "#A9C7E8", "#A6D9C3", "#F5D7A0", "#CBB3E6", "#F2A6A0", "#A6E0DC", "#F7E8A6" };
        for (int i = 0; i < 7; i++)
            cfg.Profiles.Add(new Profile { Preset = i, Name = names[i], Color = pastel[i] });
        cfg.Profiles.Add(
                new Profile
                {
                    Preset = 7, Name = "DECK", Color = "#EFB6CE",
                    Keys =
                    {
                        new KeyBinding { Type = "focusOrLaunch", Target = "notepad",  Label = "Notepad" },
                        new KeyBinding { Type = "focusOrLaunch", Target = "calc",     Label = "Calc" },
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
                    },
                });
        File.WriteAllText(FilePath, JsonSerializer.Serialize(cfg, JsonOpts));
    }

    public void Dispose() { _fsw.Dispose(); _debounce?.Dispose(); }
}
