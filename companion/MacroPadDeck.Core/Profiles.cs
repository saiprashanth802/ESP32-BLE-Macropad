using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MacroPadDeck;

public sealed class KeyBinding
{
    /// none | focusOrLaunch | open | run | window | shortcut | media | text
    ///      | favorite | padAction | write
    /// "write" opens the local-model rewrite menu (see WriteFlow): it captures
    /// the current selection and switches the pad to the style preset.
    /// App actions (focusOrLaunch/open/run/window) make the pad key type
    /// "host"; shortcut/media/text rewrite the pad key itself over BLE.
    /// padAction fires one of the pad's own builtin actions (ACTION_LIB) and
    /// never involves the host at all.
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
    /// padAction: id from the pad's ACTION_LIB, fetched live via PadActions.
    /// Stored as the id rather than the label so a renamed label still works.
    public int ActionId { get; set; }
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

    /// Sources whose playback counts as *music*, which is what drives the
    /// pad's bob and new-track reaction. Matched case-insensitively as a
    /// substring of the SMTC/MPRIS source id.
    ///
    /// An allowlist rather than a browser blocklist on purpose: a browser can
    /// be playing a song or a three-hour video and the session gives no way to
    /// tell, so browsers default to "not music". Add one here if you want the
    /// pad reacting to it. Editing profiles.json is enough — it hot-reloads.
    public List<string> MusicSources { get; set; } = new()
    {
        "feishin", "spotify", "foobar", "musicbee", "aimp",
        "tidal", "deezer", "itunes", "apple music", "winamp", "vlc",
    };

    /// True when this playback source should drive the pad's music reactions.
    /// An empty list means "everything is music", which is the pre-existing
    /// behaviour — so clearing it restores the old face rather than silencing it.
    public bool IsMusicSource(string sourceId)
    {
        if (MusicSources.Count == 0) return true;
        if (string.IsNullOrWhiteSpace(sourceId)) return false;
        foreach (string m in MusicSources)
            if (m.Length > 0 && sourceId.Contains(m, StringComparison.OrdinalIgnoreCase))
                return true;
        return false;
    }
    /// Optional Feishin Remote fallback, OFF by default. Only needed if
    /// Feishin's own "mediaSession" setting is disabled — with it on, Feishin
    /// publishes to Windows media sessions like any other player and this
    /// path is redundant. Set the URL (and the remote credentials) to enable.
    public string FeishinUrl { get; set; } = "";
    public string FeishinUser { get; set; } = "";
    public string FeishinPassword { get; set; } = "";
    public List<Profile> Profiles { get; set; } = new();
}

/// Owns %USERPROFILE%\MacroPadDeck\profiles.json. Watches it and hot-reloads,
/// so editing the file in any editor reconfigures the running app.
///
/// The store lives directly under the user profile — deliberately OUTSIDE
/// %APPDATA% — because Windows virtualizes AppData\Roaming for packaged/sandboxed
/// parent processes (e.g. launching from inside an MSIX app's terminal). That
/// redirect made the app read a *different*, empty config depending on how it was
/// launched, which looked exactly like lost key assignments. A path outside the
/// AppData subtree resolves identically no matter how the process is started.
public sealed class ProfileStore : IDisposable
{
    public static readonly string Dir =
        Path.Combine(Environment.GetEnvironmentVariable("USERPROFILE")
                     ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "MacroPadDeck");
    public static readonly string FilePath = Path.Combine(Dir, "profiles.json");

    /// Pre-pin location; migrated once into Dir on first run so existing configs carry over.
    static readonly string LegacyFilePath =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MacroPadDeck", "profiles.json");

    /// Shared serializer settings so every editor round-trips profiles.json
    /// identically (camelCase, indented, tolerant of comments/trailing commas).
    public static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingDefault,
        AllowTrailingCommas = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    /// Sample bindings for the 8th ("DECK") preset written on first run. The
    /// set is platform-specific (Windows launches notepad/calc; Linux uses
    /// xdg-open and native apps), so the front-end supplies it before the store
    /// is constructed. Null → the preset is written with no bindings.
    public static Func<List<KeyBinding>>? DeckSampleKeys;

    readonly FileSystemWatcher _fsw;
    public DeckConfig Config { get; private set; } = new();
    public event Action? Reloaded;

    public ProfileStore()
    {
        Directory.CreateDirectory(Dir);
        // One-time migration from the old %APPDATA% location. Copy rather than move
        // so an accidental sandboxed launch can never destroy the original.
        if (!File.Exists(FilePath) && File.Exists(LegacyFilePath))
        {
            try { File.Copy(LegacyFilePath, FilePath); } catch { /* fall through to WriteDefault */ }
        }
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
        cfg.Profiles.Add(new Profile
        {
            Preset = 7, Name = "DECK", Color = "#EFB6CE",
            Keys = DeckSampleKeys?.Invoke() ?? new List<KeyBinding>(),
        });
        File.WriteAllText(FilePath, JsonSerializer.Serialize(cfg, JsonOpts));
    }

    public void Dispose() { _fsw.Dispose(); _debounce?.Dispose(); }
}
