using System.IO;
using System.Text.Json;

namespace MacroPadDeck;

/// One event → emote rule. Chance 0 turns the event off; the cooldown is
/// spent on every attempt, won or lost, so a continuous condition rolls once
/// per period rather than on every tick.
public sealed class EmoteRule
{
    public string Emote { get; set; } = "happy";
    public int Chance { get; set; } = 100;
    public int CooldownS { get; set; } = 10;

    public EmoteRule() { }
    public EmoteRule(string emote, int chance, int cooldownS)
    { Emote = emote; Chance = chance; CooldownS = cooldownS; }
}

/// face.json — which emote plays for which event, and how much the face
/// dances. Pad rules are pushed to the pad (it fires them itself, PC or not);
/// host rules are fired from here by EmoteDirector.
public sealed class FaceConfig
{
    /// 0 = no dance, ~30 = on-beat bob only, 100 = every move incl. headbang.
    public int DanceLevel { get; set; } = 60;
    /// Bars between flair-emote chances while dancing; 0 = never.
    public int FlairBars { get; set; } = 8;
    /// Loudness jump (0-1 scale) over the last few seconds that counts as a drop.
    public double DropJump { get; set; } = 0.25;

    /// Keyed by Protocol.PadTriggers names.
    public Dictionary<string, EmoteRule> Pad { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    /// "drop", "late", and "app:<context>" for each context in moods.json.
    public Dictionary<string, EmoteRule> Host { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    /// Firmware defaults (emoteMap[] in macropad_v5.ino) — keep in step.
    public static FaceConfig Defaults() => new()
    {
        Pad = new(StringComparer.OrdinalIgnoreCase)
        {
            ["boot"]       = new("happy", 100, 0),
            ["connect"]    = new("happy", 100, 5),
            ["disconnect"] = new("sad", 100, 5),
            ["typing"]     = new("focused", 60, 20),
            ["idle"]       = new("sleepy", 60, 120),
            ["wake"]       = new("surprised", 100, 0),
            ["preset"]     = new("wink", 70, 10),
            ["slot"]       = new("wink", 100, 0),
            ["track"]      = new("surprised", 80, 20),
            ["favourite"]  = new("love", 100, 0),
            ["paused"]     = new("neutral", 0, 30),
            ["flair"]      = new(Protocol.EmoteRandom, 70, 0),
        },
        Host = new(StringComparer.OrdinalIgnoreCase)
        {
            ["drop"]       = new("joy", 80, 30),
            ["late"]       = new("tired", 100, 3600),
            ["app:play"]   = new("angry", 60, 300),
            ["app:focus"]  = new("focused", 50, 300),
            ["app:social"] = new("happy", 40, 300),
            ["app:watch"]  = new("vibing", 0, 300),
            ["app:browse"] = new("skeptical", 0, 300),
        },
    };

    /// Fill any rule the file is missing (an older face.json, a new trigger).
    public void FillMissing()
    {
        var d = Defaults();
        Pad = new(Pad, StringComparer.OrdinalIgnoreCase);
        Host = new(Host, StringComparer.OrdinalIgnoreCase);
        foreach (var (k, v) in d.Pad) Pad.TryAdd(k, v);
        foreach (var (k, v) in d.Host) Host.TryAdd(k, v);
    }

    /// The pad table in trigger order, as wire ids.
    public List<(int trigger, int emote, int chance, int cooldownS)> PadMap()
    {
        var list = new List<(int, int, int, int)>();
        for (int t = 0; t < Protocol.PadTriggers.Length; t++)
        {
            if (!Pad.TryGetValue(Protocol.PadTriggers[t], out var r)) continue;
            int id = Protocol.EmoteId(r.Emote);
            if (id < 0) continue;                       // unknown name: leave the pad's entry
            list.Add((t, id, r.Chance, r.CooldownS));
        }
        return list;
    }
}

public sealed class FaceStore : IDisposable
{
    public static readonly string FilePath = Path.Combine(ProfileStore.Dir, "face.json");

    readonly FileSystemWatcher _fsw;
    public FaceConfig Config { get; private set; } = FaceConfig.Defaults();
    public event Action? Reloaded;

    public FaceStore()
    {
        Directory.CreateDirectory(ProfileStore.Dir);
        if (!File.Exists(FilePath)) Save(FaceConfig.Defaults());
        Load();
        _fsw = new FileSystemWatcher(ProfileStore.Dir, "face.json") { EnableRaisingEvents = true };
        _fsw.Changed += (_, _) => DebouncedReload();
        _fsw.Created += (_, _) => DebouncedReload();
    }

    System.Threading.Timer? _debounce;
    void DebouncedReload() =>
        (_debounce ??= new System.Threading.Timer(_ =>
        {
            Load();
            Diag.Log("face: face.json reloaded");
            Reloaded?.Invoke();
        })).Change(400, Timeout.Infinite);

    void Load()
    {
        try
        {
            using var fs = new FileStream(FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var c = JsonSerializer.Deserialize<FaceConfig>(fs, StyleStore.JsonOpts) ?? FaceConfig.Defaults();
            c.FillMissing();
            Config = c;
        }
        catch { /* mid-save or bad JSON — keep the last good config */ }
    }

    /// A deep copy for the editor to change without touching the live config.
    public FaceConfig Copy() =>
        JsonSerializer.Deserialize<FaceConfig>(JsonSerializer.Serialize(Config, StyleStore.JsonOpts),
                                               StyleStore.JsonOpts)!.Also(c => c.FillMissing());

    /// Writes face.json and takes a copy as the live config, so the caller can
    /// keep editing its own object without changing what is live.
    public void Save(FaceConfig c)
    {
        string json = JsonSerializer.Serialize(c, StyleStore.JsonOpts);
        File.WriteAllText(FilePath, json);
        var live = JsonSerializer.Deserialize<FaceConfig>(json, StyleStore.JsonOpts)!;
        live.FillMissing();
        Config = live;
    }

    public void Dispose() { _fsw.Dispose(); _debounce?.Dispose(); }
}

static class FaceConfigExt
{
    public static FaceConfig Also(this FaceConfig c, Action<FaceConfig> f) { f(c); return c; }
}
