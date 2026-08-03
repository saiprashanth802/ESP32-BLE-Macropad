using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MacroPadDeck;

/// Outcome of one rewrite. Subject is only populated for structured styles.
public readonly record struct LlmResult(bool Ok, string Text, string Subject, string Error)
{
    public static LlmResult Fail(string e) => new(false, "", "", e);
}

/// Talks to a local Ollama instance. Every method swallows its exceptions and
/// reports failure in the return value — a dead model server must never take
/// down the tray app, and "not running" is the normal state here rather than
/// an error worth throwing over.
///
/// Wire format is Ollama's, which is snake_case — deliberately NOT using
/// ProfileStore.JsonOpts (camelCase) for these DTOs.
public sealed class LlmClient : IDisposable
{
    readonly HttpClient _http;
    readonly Func<WriteConfig> _cfg;

    public LlmClient(Func<WriteConfig> cfg)
    {
        _cfg = cfg;
        _http = new HttpClient { Timeout = TimeSpan.FromSeconds(Math.Max(30, cfg().RequestTimeoutSeconds)) };
    }

    string Base => _cfg().Endpoint.TrimEnd('/');
    static void Log(string m) => Diag.Log($"llm: {m}");

    static readonly JsonSerializerOptions Wire = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    /// keep_alive is a Go duration on the wire: a bare number means seconds
    /// (negative = keep loaded forever), but a *string* must carry a unit.
    /// Sending "-1" as a string fails every request with
    /// `time: missing unit in duration "-1"`, so numeric values are emitted as
    /// JSON numbers and anything else ("30m", "1h") passes through as a string.
    static object? KeepAliveValue(string s)
    {
        s = (s ?? "").Trim();
        if (s.Length == 0) return null;
        return long.TryParse(s, out long n) ? n : s;
    }

    // ---- server / model state -------------------------------------------------

    /// Is the Ollama server reachable at all? Cheap enough to call on every
    /// trigger press.
    /// A cold Ollama server takes ~2 s to answer its first request (measured),
    /// and longer while it is busy pulling a model — so this is generous and
    /// retried once. Too tight a bound here reads as "not running" on the first
    /// press after an idle spell, which is exactly when you reach for it.
    public async Task<bool> IsUp()
    {
        for (int attempt = 0; attempt < 2; attempt++)
        {
            try
            {
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(6));
                var r = await _http.GetAsync($"{Base}/api/tags", cts.Token);
                if (r.IsSuccessStatusCode) return true;
                Log($"tags returned {(int)r.StatusCode}");
            }
            catch (Exception ex) { Log($"tags attempt {attempt + 1} failed: {ex.Message}"); }
        }
        return false;
    }

    /// Ollama serves its hosted models through the same local endpoint, marked
    /// by a "-cloud" tag. They occupy no VRAM here and never appear in /api/ps.
    public static bool IsCloud(string model) =>
        model.EndsWith("-cloud", StringComparison.OrdinalIgnoreCase);

    /// Is this model currently resident (i.e. occupying VRAM)? /api/ps lists
    /// only loaded models, which is exactly the question the pad's status line
    /// wants answered before it promises an instant rewrite.
    public async Task<bool> IsLoaded(string model)
    {
        // A cloud model is always "ready": there is nothing to load, and /api/ps
        // will never list it — so without this the auto-load path fires on every
        // single request, flashing LOADING MODEL on the pad for a no-op call.
        if (IsCloud(model)) return true;
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            var r = await _http.GetAsync($"{Base}/api/ps", cts.Token);
            if (!r.IsSuccessStatusCode) return false;
            using var doc = JsonDocument.Parse(await r.Content.ReadAsStringAsync(cts.Token));
            if (!doc.RootElement.TryGetProperty("models", out var arr)) return false;
            foreach (var m in arr.EnumerateArray())
                if (m.TryGetProperty("name", out var n) && NameMatches(n.GetString(), model))
                    return true;
            return false;
        }
        catch { return false; }
    }

    /// Ollama reports "qwen3:8b" but a config may say "qwen3" — treat a bare
    /// name as matching its :latest-style tag so a reasonable config still works.
    static bool NameMatches(string? reported, string wanted)
    {
        if (reported is null) return false;
        if (reported.Equals(wanted, StringComparison.OrdinalIgnoreCase)) return true;
        return !wanted.Contains(':')
            && reported.StartsWith(wanted + ":", StringComparison.OrdinalIgnoreCase);
    }

    /// Pull the model into VRAM without generating anything. An empty-prompt
    /// /api/generate is Ollama's documented load path.
    public async Task<bool> Load(string model)
    {
        try
        {
            var body = JsonSerializer.Serialize(
                new GenReq { Model = model, KeepAlive = KeepAliveValue(_cfg().KeepAlive) }, Wire);
            var r = await _http.PostAsync($"{Base}/api/generate",
                new StringContent(body, Encoding.UTF8, "application/json"));
            Log($"load {model}: {(int)r.StatusCode}");
            return r.IsSuccessStatusCode;
        }
        catch (Exception ex) { Log($"load {model} failed: {ex.Message}"); return false; }
    }

    /// Release the VRAM. keep_alive 0 drops the model as soon as the (empty)
    /// request completes.
    public async Task<bool> Unload(string model)
    {
        if (IsCloud(model)) return true;      // nothing local to release
        try
        {
            var body = JsonSerializer.Serialize(
                new GenReq { Model = model, KeepAlive = 0L }, Wire);
            var r = await _http.PostAsync($"{Base}/api/generate",
                new StringContent(body, Encoding.UTF8, "application/json"));
            Log($"unload {model}: {(int)r.StatusCode}");
            return r.IsSuccessStatusCode;
        }
        catch (Exception ex) { Log($"unload {model} failed: {ex.Message}"); return false; }
    }

    // ---- generation -----------------------------------------------------------

    /// Run one style over one piece of text.
    ///
    /// Plain styles stream so the preview fills in as it goes (2-4 s of dead
    /// air feels broken; the same wait feels fine when you can watch it).
    /// Structured styles can't stream — partial JSON isn't parseable — so they
    /// buffer and parse once.
    public async Task<LlmResult> Run(WriteStyle style, string model, string input,
                                     Action<string>? onDelta, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(input)) return LlmResult.Fail("Nothing selected");

        var msgs = new List<ChatMsg> { new("system", style.System) };
        foreach (var ex in style.Examples)
        {
            if (ex.In.Length == 0 || ex.Out.Length == 0) continue;
            msgs.Add(new("user", ex.In));
            msgs.Add(new("assistant", ex.Out));
        }
        msgs.Add(new("user", input));

        var req = new ChatReq
        {
            Model = model,
            Messages = msgs,
            Stream = !style.Structured,
            KeepAlive = KeepAliveValue(_cfg().KeepAlive),
            Options = new ChatOpts { Temperature = style.Temperature },
            Format = style.Structured ? SubjectBodySchema : null,
            // Per-style wins; otherwise follow the global setting.
            Think = style.Think ?? (_cfg().DisableThinking ? false : null),
        };

        var (ok, raw, err) = await Send(req, onDelta, ct);

        // A model with no reasoning pass rejects `think` outright. Drop it and
        // try once more rather than failing an otherwise valid request — this
        // is what lets a small non-thinking model be used for the cheap styles.
        if (!ok && req.Think is not null &&
            err.Contains("think", StringComparison.OrdinalIgnoreCase))
        {
            Log("model rejected `think` — retrying without it");
            req.Think = null;
            (ok, raw, err) = await Send(req, onDelta, ct);
        }

        if (!ok) return LlmResult.Fail(err);
        return style.Structured ? ParseStructured(raw) : new LlmResult(true, Clean(raw), "", "");
    }

    async Task<(bool Ok, string Raw, string Error)> Send(ChatReq req, Action<string>? onDelta,
                                                        CancellationToken ct)
    {
        try
        {
            using var msg = new HttpRequestMessage(HttpMethod.Post, $"{Base}/api/chat")
            {
                Content = new StringContent(JsonSerializer.Serialize(req, Wire),
                                            Encoding.UTF8, "application/json"),
            };
            using var resp = await _http.SendAsync(msg, HttpCompletionOption.ResponseHeadersRead, ct);
            if (!resp.IsSuccessStatusCode)
            {
                // The body carries the real reason ("missing unit in duration",
                // "does not support thinking"); a bare status code does not.
                string body = await resp.Content.ReadAsStringAsync(ct);
                Log($"chat {(int)resp.StatusCode}: {Head(body, 200)}");
                return (false, "", ErrorText(body, (int)resp.StatusCode));
            }

            string raw = req.Stream
                ? await ReadStream(resp, onDelta, ct)
                : ReadOne(await resp.Content.ReadAsStringAsync(ct));
            return (true, raw, "");
        }
        catch (OperationCanceledException) { return (false, "", "Cancelled"); }
        catch (Exception ex) { Log($"chat failed: {ex.Message}"); return (false, "", ex.Message); }
    }

    static string ErrorText(string body, int status)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            if (doc.RootElement.TryGetProperty("error", out var e) && e.GetString() is { } s)
                return s;
        }
        catch { /* not JSON */ }
        return $"Model server returned {status}";
    }

    static string Head(string s, int max) => s.Length <= max ? s : s[..max];

    /// NDJSON: one JSON object per line, each carrying a content delta.
    static async Task<string> ReadStream(HttpResponseMessage resp, Action<string>? onDelta,
                                         CancellationToken ct)
    {
        var sb = new StringBuilder();
        using var stream = await resp.Content.ReadAsStreamAsync(ct);
        using var reader = new StreamReader(stream, Encoding.UTF8);
        while (await reader.ReadLineAsync(ct) is { } line)
        {
            if (line.Length == 0) continue;
            string? delta = null;
            try
            {
                using var doc = JsonDocument.Parse(line);
                if (doc.RootElement.TryGetProperty("message", out var m) &&
                    m.TryGetProperty("content", out var c))
                    delta = c.GetString();
            }
            catch { continue; }          // a truncated line is not worth failing over
            if (string.IsNullOrEmpty(delta)) continue;
            sb.Append(delta);
            onDelta?.Invoke(delta);
        }
        return sb.ToString();
    }

    static string ReadOne(string body)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            return doc.RootElement.TryGetProperty("message", out var m) &&
                   m.TryGetProperty("content", out var c)
                ? c.GetString() ?? "" : "";
        }
        catch { return ""; }
    }

    static LlmResult ParseStructured(string raw)
    {
        string cleaned = Clean(raw);
        try
        {
            using var doc = JsonDocument.Parse(cleaned);
            string subject = doc.RootElement.TryGetProperty("subject", out var s) ? s.GetString() ?? "" : "";
            string body    = doc.RootElement.TryGetProperty("body", out var b) ? b.GetString() ?? "" : "";
            if (body.Length == 0) return LlmResult.Fail("Model returned no body");
            return new LlmResult(true, body.Replace("\r\n", "\n"), subject, "");
        }
        catch
        {
            // Schema-constrained decoding should make this unreachable, but a
            // model that ignored the schema is better surfaced as usable prose
            // than as an error the user can do nothing about.
            Log("structured parse failed — falling back to raw text");
            return cleaned.Length > 0
                ? new LlmResult(true, cleaned, "", "")
                : LlmResult.Fail("Model returned nothing");
        }
    }

    // ---- output hygiene -------------------------------------------------------

    /// Strip the three things small models add that you never want pasted:
    /// reasoning traces, a "Here's the rewrite:" lead-in, and wrapping
    /// quotes/code fences. Instructing against these is not reliable on its
    /// own — belt and braces is the only thing that actually holds.
    public static string Clean(string s)
    {
        if (string.IsNullOrEmpty(s)) return "";

        // Thinking models (qwen3 among them) can inline a reasoning block.
        // Newer Ollama splits it into message.thinking, older versions do not.
        for (int guard = 0; guard < 8; guard++)
        {
            int a = s.IndexOf("<think>", StringComparison.OrdinalIgnoreCase);
            if (a < 0) break;
            int b = s.IndexOf("</think>", a, StringComparison.OrdinalIgnoreCase);
            if (b < 0) { s = s[..a]; break; }        // unterminated: drop the tail
            s = s.Remove(a, b + "</think>".Length - a);
        }

        // A degenerating model can emit a long run of unmatched closing tags
        // with no opener — observed for real when a structured response was
        // truncated mid-string. The paired pass above cannot see those, so
        // sweep any survivors rather than letting them reach the preview.
        s = s.Replace("</think>", "", StringComparison.OrdinalIgnoreCase)
             .Replace("<think>", "", StringComparison.OrdinalIgnoreCase);

        s = s.Replace("\r\n", "\n").Trim();

        // ```lang … ``` fences
        if (s.StartsWith("```"))
        {
            int nl = s.IndexOf('\n');
            int close = s.LastIndexOf("```", StringComparison.Ordinal);
            if (nl > 0 && close > nl) s = s[(nl + 1)..close].Trim();
        }

        // A short first line ending in ':' with body after it is a lead-in
        // ("Here is the formal version:"), not content.
        int firstBreak = s.IndexOf('\n');
        if (firstBreak > 0 && firstBreak < 80)
        {
            string head = s[..firstBreak].TrimEnd();
            if (head.EndsWith(':') && !head.StartsWith('{'))
                s = s[(firstBreak + 1)..].TrimStart();
        }

        // Whole-output wrapping quotes.
        if (s.Length >= 2)
        {
            char f = s[0], l = s[^1];
            if ((f == '"' && l == '"') || (f == '\'' && l == '\'') ||
                (f == '“' && l == '”'))
                s = s[1..^1].Trim();
        }

        return s;
    }

    // ---- wire DTOs ------------------------------------------------------------

    static readonly JsonElement SubjectBodySchema = JsonDocument.Parse("""
        {
          "type": "object",
          "properties": {
            "subject": { "type": "string" },
            "body":    { "type": "string" }
          },
          "required": ["subject", "body"]
        }
        """).RootElement.Clone();

    sealed record ChatMsg(
        [property: JsonPropertyName("role")]    string Role,
        [property: JsonPropertyName("content")] string Content);

    sealed class ChatOpts
    {
        [JsonPropertyName("temperature")] public double Temperature { get; set; }
    }

    sealed class ChatReq
    {
        [JsonPropertyName("model")]      public string Model { get; set; } = "";
        [JsonPropertyName("messages")]   public List<ChatMsg> Messages { get; set; } = new();
        [JsonPropertyName("stream")]     public bool Stream { get; set; }
        [JsonPropertyName("keep_alive")] public object? KeepAlive { get; set; }
        [JsonPropertyName("options")]    public ChatOpts? Options { get; set; }
        [JsonPropertyName("format")]     public JsonElement? Format { get; set; }
        /// Omitted entirely when null — a non-thinking model rejects the field.
        [JsonPropertyName("think")]      public bool? Think { get; set; }
    }

    sealed class GenReq
    {
        [JsonPropertyName("model")]      public string Model { get; set; } = "";
        [JsonPropertyName("keep_alive")] public object? KeepAlive { get; set; }
    }

    public void Dispose() => _http.Dispose();
}
