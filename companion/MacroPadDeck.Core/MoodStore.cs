using System.IO;
using System.Text.Json;

namespace MacroPadDeck;

/// A point on the face's mood map. Valence (−100 sad … +100 happy) and
/// arousal (−100 drowsy … +100 wired), the same units HCMD_MOOD carries.
public sealed class MoodPoint
{
    public int V { get; set; }
    public int A { get; set; }
    public bool Focused { get; set; }
}

/// moods.json — everything the mood engine maps from, hot-reloaded like
/// styles.json so tuning never needs a rebuild.
public sealed class MoodConfig
{
    public bool Enabled { get; set; } = true;
    /// Listen to system audio (WASAPI loopback) for loudness and the beat.
    public bool Audio { get; set; } = true;

    /// How far the pad leans toward the companion's mood (0-100). Music is the
    /// strongest evidence we have; app context alone is only a hint, so the
    /// pad's own mood (typing rate, link health) keeps a bigger say there.
    public int WeightMusic { get; set; } = 75;
    public int WeightContext { get; set; } = 45;
    public int TtlSeconds { get; set; } = 90;

    /// Beat-clock timing. The pad's anchor is shifted by BLE delivery latency
    /// (later) and by the output device's latency — Bluetooth headphones can
    /// be 150-250 ms behind the loopback tap, and the nod should match what
    /// you hear, not what Windows mixed.
    public int BleLatencyMs { get; set; } = 30;
    public int OutputLatencyMs { get; set; } = 0;

    /// Hours counted as "late" (start inclusive, end exclusive, may wrap).
    public int LateFrom { get; set; } = 23;
    public int LateTo { get; set; } = 6;

    /// Genre keyword → mood. Matched case-insensitively as a substring of each
    /// tag; the longest matching key wins, so "death metal" beats "metal".
    public Dictionary<string, MoodPoint> Genres { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    /// Used for music with no matching tag.
    public MoodPoint UnknownMusic { get; set; } = new() { V = 20, A = 15 };

    /// Named contexts, and which process (exe name, no .exe) means which.
    public Dictionary<string, MoodPoint> Contexts { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public Dictionary<string, string> Apps { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

public sealed class MoodStore : IDisposable
{
    public static readonly string FilePath = Path.Combine(ProfileStore.Dir, "moods.json");

    readonly FileSystemWatcher _fsw;
    public MoodConfig Config { get; private set; } = new();

    public MoodStore()
    {
        Directory.CreateDirectory(ProfileStore.Dir);
        if (!File.Exists(FilePath)) WriteDefault();
        Load();
        _fsw = new FileSystemWatcher(ProfileStore.Dir, "moods.json") { EnableRaisingEvents = true };
        _fsw.Changed += (_, _) => DebouncedReload();
        _fsw.Created += (_, _) => DebouncedReload();
    }

    System.Threading.Timer? _debounce;
    void DebouncedReload() =>
        (_debounce ??= new System.Threading.Timer(_ =>
        {
            Load();
            Diag.Log($"moods: reloaded ({Config.Genres.Count} genres, {Config.Apps.Count} apps)");
        })).Change(400, Timeout.Infinite);

    void Load()
    {
        try
        {
            using var fs = new FileStream(FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var c = JsonSerializer.Deserialize<MoodConfig>(fs, StyleStore.JsonOpts) ?? new MoodConfig();
            // System.Text.Json builds plain dictionaries — restore case-insensitive lookup
            c.Genres = new(c.Genres, StringComparer.OrdinalIgnoreCase);
            c.Contexts = new(c.Contexts, StringComparer.OrdinalIgnoreCase);
            c.Apps = new(c.Apps, StringComparer.OrdinalIgnoreCase);
            Config = c;
        }
        catch { /* mid-save or bad JSON — keep the last good config */ }
    }

    void WriteDefault() =>
        File.WriteAllText(FilePath, JsonSerializer.Serialize(Defaults(), StyleStore.JsonOpts));

    static MoodPoint P(int v, int a, bool focused = false) => new() { V = v, A = a, Focused = focused };

    // Starting points, meant to be tuned by hand. Valence is how bright the
    // music feels, arousal how driving — not a claim about the genre's merit.
    static MoodConfig Defaults() => new()
    {
        Genres = new(StringComparer.OrdinalIgnoreCase)
        {
            ["metal"] = P(-10, 90),       ["death metal"] = P(-35, 95), ["doom"] = P(-45, 20),
            ["punk"] = P(20, 85),         ["rock"] = P(20, 60),         ["hard rock"] = P(10, 75),
            ["indie"] = P(25, 30),        ["alternative"] = P(10, 40),  ["grunge"] = P(-20, 55),
            ["pop"] = P(60, 45),          ["k-pop"] = P(70, 65),        ["hyperpop"] = P(55, 90),
            ["dance"] = P(60, 75),        ["house"] = P(50, 65),        ["techno"] = P(10, 80),
            ["trance"] = P(45, 70),       ["drum and bass"] = P(25, 90),["dnb"] = P(25, 90),
            ["dubstep"] = P(0, 90),       ["edm"] = P(55, 80),          ["electronic"] = P(25, 55),
            ["synthwave"] = P(35, 50),    ["ambient"] = P(20, -60),     ["lo-fi"] = P(30, -40),
            ["lofi"] = P(30, -40),        ["chill"] = P(40, -35),       ["downtempo"] = P(20, -30),
            ["hip hop"] = P(15, 55),      ["hip-hop"] = P(15, 55),      ["rap"] = P(10, 60),
            ["trap"] = P(0, 70),          ["r&b"] = P(40, 20),          ["soul"] = P(45, 15),
            ["funk"] = P(65, 60),         ["disco"] = P(75, 70),        ["jazz"] = P(40, 5),
            ["blues"] = P(-25, 0),        ["classical"] = P(25, -25),   ["soundtrack"] = P(15, 10),
            ["score"] = P(10, 5),         ["folk"] = P(30, -10),        ["acoustic"] = P(30, -25),
            ["country"] = P(40, 25),      ["reggae"] = P(55, 10),       ["latin"] = P(65, 60),
            ["emo"] = P(-50, 55),         ["sad"] = P(-60, -30),        ["shoegaze"] = P(-10, 10),
            ["carnatic"] = P(35, 20),     ["hindustani"] = P(30, 0),    ["bollywood"] = P(60, 50),
            ["filmi"] = P(55, 40),
        },
        Contexts = new(StringComparer.OrdinalIgnoreCase)
        {
            ["focus"]  = P(10, 15, focused: true),
            ["social"] = P(35, 25),
            ["browse"] = P(5, 0),
            ["play"]   = P(45, 70),
            ["watch"]  = P(20, -10),
        },
        Apps = new(StringComparer.OrdinalIgnoreCase)
        {
            ["code"] = "focus", ["devenv"] = "focus", ["rider64"] = "focus",
            ["windowsterminal"] = "focus", ["claude"] = "focus", ["arduino ide"] = "focus",
            ["discord"] = "social", ["whatsapp"] = "social", ["telegram"] = "social",
            ["chrome"] = "browse", ["msedge"] = "browse", ["firefox"] = "browse",
            ["steam"] = "play", ["steamwebhelper"] = "play",
            ["vlc"] = "watch", ["mpv"] = "watch", ["jellyfin media player"] = "watch",
        },
    };

    public void Dispose() { _fsw.Dispose(); _debounce?.Dispose(); }
}
