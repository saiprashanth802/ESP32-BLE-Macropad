using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace MacroPadDeck;

/// Reads now-playing from Feishin's Remote server (HTTP Basic auth), used as
/// a fallback when Windows' media sessions report nothing — Feishin does not
/// publish to SMTC, so this is the only way to see its playback.
///
/// Credentials come from profiles.json (feishinUser / feishinPassword); they
/// are never stored anywhere else. The endpoint and JSON shape are discovered
/// at runtime: Feishin's remote API has changed across releases, so we probe
/// a candidate list and pull recognised fields wherever they appear.
public sealed class FeishinSource
{
    static readonly string[] Candidates =
    {
        "/api/state", "/api/playback", "/api/song", "/api/nowplaying",
        "/api/current", "/api/status", "/state", "/nowplaying",
    };

    readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(4) };
    readonly string _baseUrl;
    string? _endpoint;             // remembered once something works
    bool _warned;

    public FeishinSource(string baseUrl, string user, string password)
    {
        _baseUrl = baseUrl.TrimEnd('/');
        if (user.Length > 0 || password.Length > 0)
            _http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue(
                "Basic", Convert.ToBase64String(Encoding.UTF8.GetBytes($"{user}:{password}")));
    }

    static void Log(string m)
    {
        try { File.AppendAllText(Path.Combine(ProfileStore.Dir, "deck.log"),
                                 $"{DateTime.Now:HH:mm:ss.fff} feishin: {m}\r\n"); } catch { }
    }

    public async Task<(bool ok, string title, int pos, int dur, bool playing)> Poll()
    {
        foreach (string path in _endpoint is null ? Candidates : new[] { _endpoint })
        {
            try
            {
                var resp = await _http.GetAsync(_baseUrl + path);
                if (resp.StatusCode == System.Net.HttpStatusCode.Unauthorized)
                {
                    if (!_warned) { _warned = true; Log("401 — set feishinUser/feishinPassword in profiles.json"); }
                    return default;
                }
                if (!resp.IsSuccessStatusCode) continue;
                string body = await resp.Content.ReadAsStringAsync();
                if (!body.TrimStart().StartsWith('{')) continue;

                using var doc = JsonDocument.Parse(body);
                string title = FindString(doc.RootElement, "title", "name", "song", "track") ?? "";
                if (title.Length == 0) continue;

                if (_endpoint != path) { _endpoint = path; Log($"using {path}"); }

                double pos = FindNumber(doc.RootElement, "position", "positionsec", "elapsed", "currenttime", "progress") ?? 0;
                double dur = FindNumber(doc.RootElement, "duration", "durationsec", "length", "totaltime") ?? 0;
                // Some builds report milliseconds — normalise anything implausibly large
                if (dur > 100000) { dur /= 1000; pos /= 1000; }
                bool playing = FindBool(doc.RootElement, "isplaying", "playing") ??
                               (FindString(doc.RootElement, "status", "state") ?? "").ToLowerInvariant() == "playing";
                return (true, title, (int)pos, (int)dur, playing);
            }
            catch { /* server sleeping or endpoint absent — try the next */ }
        }
        return default;
    }

    // ── tolerant JSON field lookup (case-insensitive, recurses one level) ──
    static string? FindString(JsonElement e, params string[] keys) =>
        Find(e, keys, JsonValueKind.String)?.GetString();

    static double? FindNumber(JsonElement e, params string[] keys)
    {
        var v = Find(e, keys, JsonValueKind.Number);
        return v?.GetDouble();
    }

    static bool? FindBool(JsonElement e, params string[] keys)
    {
        var v = Find(e, keys, JsonValueKind.True) ?? Find(e, keys, JsonValueKind.False);
        return v?.GetBoolean();
    }

    static JsonElement? Find(JsonElement e, string[] keys, JsonValueKind kind, int depth = 0)
    {
        if (e.ValueKind != JsonValueKind.Object || depth > 2) return null;
        foreach (var prop in e.EnumerateObject())
        {
            string n = prop.Name.ToLowerInvariant();
            if (prop.Value.ValueKind == kind && keys.Contains(n)) return prop.Value;
        }
        foreach (var prop in e.EnumerateObject())      // then descend
        {
            var hit = Find(prop.Value, keys, kind, depth + 1);
            if (hit is not null) return hit;
        }
        return null;
    }
}
