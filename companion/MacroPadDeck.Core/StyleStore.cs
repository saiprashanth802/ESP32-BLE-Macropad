using System.IO;
using System.Text.Json;

namespace MacroPadDeck;

/// One worked example pinned into a style's prompt. Few-shot examples move
/// small models far more than longer instructions do — two or three in the
/// user's own register beat any amount of describing that register.
public sealed class StyleExample
{
    public string In { get; set; } = "";
    public string Out { get; set; } = "";
}

/// One entry in the pad's style menu: a key position, a label the pad can
/// draw, and the prompt that defines the transform.
public sealed class WriteStyle
{
    /// Pad key index 0..11 this style occupies while the menu is up.
    public int Key { get; set; }
    /// Pad label, ASCII, ≤8 chars (Protocol.SetLabel truncates beyond that).
    public string Label { get; set; } = "";
    public string System { get; set; } = "";
    /// Per-style override; falls back to WriteConfig.Model when empty. Lets the
    /// expensive EMAIL transform point at a bigger model than the cheap ones.
    public string Model { get; set; } = "";
    /// 0 is stilted for prose work — a little sampling reads far more natural.
    public double Temperature { get; set; } = 0.3;
    /// Per-style override of WriteConfig.DisableThinking; null follows it.
    ///
    /// Measured: tone rewrites are 2.4x faster with thinking off and no worse.
    /// Proofreading is the opposite — with thinking off, qwen3:8b caught 4 of
    /// 13 planted typos; with it on, all 13. Careful exhaustive checking is
    /// what the reasoning pass is actually good for.
    public bool? Think { get; set; }
    /// Ask for {subject, body} instead of raw prose. Only EMAIL wants this: a
    /// generated subject can't be pasted into a compose *body*, so it has to
    /// come back as a separate field rather than be buried in the text.
    public bool Structured { get; set; }
    public List<StyleExample> Examples { get; set; } = new();
}

public sealed class WriteConfig
{
    public string Endpoint { get; set; } = "http://localhost:11434";
    public string Model { get; set; } = "qwen3:8b";
    /// Generation can take a while on a laptop GPU with a cold model.
    public int RequestTimeoutSeconds { get; set; } = 120;
    /// The menu gives up and restores your real keys after this much silence,
    /// so a stray press can never strand the pad in a half-open menu.
    public int MenuTimeoutSeconds { get; set; } = 45;
    /// Ollama unloads after 5 minutes by default, which would drop the model
    /// mid-session and make the next key press pay a reload for no reason.
    /// "-1" pins it until an explicit unload.
    public string KeepAlive { get; set; } = "-1";
    /// Load the model on demand when a rewrite is requested and it isn't
    /// resident. Without this a few-times-a-week tool fails at exactly the
    /// moment you reach for it, and you stop reaching for it.
    public bool AutoLoad { get; set; } = true;

    /// Ask thinking models (qwen3 among them) to skip their reasoning pass.
    /// Measured on qwen3:8b: a one-sentence formalise took 12.9 s with thinking
    /// and 5.4 s without, burning ~364 reasoning tokens to produce a 25-token
    /// answer. Tone rewriting needs no deliberation, so this is close to free
    /// quality-wise. Models that don't support the flag are retried without it.
    public bool DisableThinking { get; set; } = true;
    /// Face mode to assume when the pad reports OFF on connect: 0=off, 1=idle,
    /// 2=always. The menu turns the face off while it is open, so a pad found
    /// in mode 0 almost always means a previous session was interrupted before
    /// it could restore — this is what it gets put back to.
    public int RestoreFaceMode { get; set; } = 2;

    /// Pad preset the style menu owns, or -1 to disable the feature.
    ///
    /// This preset's 12 keys are rewritten to host-notify keys on every connect
    /// (RAM only — never committed), so the app fully owns them and can restore
    /// the menu labels after a rewrite. It has to be a dedicated preset: keys
    /// left as "none" in profiles.json are firmware defaults the app has no way
    /// to read back, so clobbering an arbitrary preset would strand them until
    /// the pad reboots.
    ///
    /// Setting this back to -1 releases the preset — profiles.json is never
    /// modified, so the original layout returns on the next connect.
    public int WritePreset { get; set; } = 7;
    public List<WriteStyle> Styles { get; set; } = new();
}

/// Owns %USERPROFILE%\MacroPadDeck\styles.json, hot-reloaded like profiles.json.
///
/// The prompts live in a file rather than in code on purpose: "formal" means
/// something different to everyone, and this is the knob that actually decides
/// whether the output is usable. Editing it must not require a rebuild.
public sealed class StyleStore : IDisposable
{
    public static readonly string FilePath = Path.Combine(ProfileStore.Dir, "styles.json");

    /// Deliberately NOT ProfileStore.JsonOpts. That uses WhenWritingDefault,
    /// which drops any property equal to its type default — so a style with
    /// Temperature = 0.0 was written without the field, and on reload the
    /// property initializer put 0.3 back. The intended value never survived a
    /// round-trip, silently. Writing every field also makes the file
    /// self-documenting, which matters for one meant to be hand-tuned.
    public static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        AllowTrailingCommas = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    readonly FileSystemWatcher _fsw;
    public WriteConfig Config { get; private set; } = new();
    public event Action? Reloaded;

    public StyleStore()
    {
        Directory.CreateDirectory(ProfileStore.Dir);
        if (!File.Exists(FilePath)) WriteDefault();
        Load();
        _fsw = new FileSystemWatcher(ProfileStore.Dir, "styles.json") { EnableRaisingEvents = true };
        _fsw.Changed += (_, _) => DebouncedReload();
        _fsw.Created += (_, _) => DebouncedReload();
    }

    System.Threading.Timer? _debounce;
    void DebouncedReload() =>
        (_debounce ??= new System.Threading.Timer(_ =>
        {
            Load();
            Diag.Log($"styles: reloaded ({Config.Styles.Count} styles)");
            Reloaded?.Invoke();
        })).Change(400, Timeout.Infinite);

    void Load()
    {
        try
        {
            using var fs = new FileStream(FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            Config = JsonSerializer.Deserialize<WriteConfig>(fs, JsonOpts) ?? new WriteConfig();
        }
        catch { /* mid-save or bad JSON — keep the last good config */ }
    }

    public WriteStyle? ForKey(int key) => Config.Styles.FirstOrDefault(s => s.Key == key);

    /// Model this style actually runs on, after the per-style override.
    public string ModelFor(WriteStyle s) =>
        s.Model.Length > 0 ? s.Model : Config.Model;

    void WriteDefault() =>
        File.WriteAllText(FilePath, JsonSerializer.Serialize(Defaults(), JsonOpts));

    // Shared across every style. Small models open with "Sure! Here's a more
    // formal version:" unless told not to, and one instruction is not enough —
    // LlmClient also strips a leading label line and wrapping quotes.
    const string NoPreamble =
        "Output only the resulting text. No preamble, no commentary, no explanation, " +
        "and do not wrap the output in quotation marks.";

    const string NoInvention =
        "Use only facts present in the input. Never invent names, dates, times, " +
        "numbers, or events.";

    static WriteConfig Defaults() => new()
    {
        Styles = new List<WriteStyle>
        {
            new()
            {
                Key = 0, Label = "FORMAL",
                System =
                    "You rewrite text in a formal, professional register. " +
                    "Preserve the author's meaning and structure. " + NoInvention + " " +
                    "Keep the result within roughly 15% of the original length — do not pad. " +
                    "Preserve line breaks and list structure. " + NoPreamble,
                Examples =
                {
                    new StyleExample
                    {
                        In  = "hey just checking if you got my last mail about the lab slot",
                        Out = "I wanted to follow up on my previous email regarding the lab slot.",
                    },
                },
            },
            new()
            {
                Key = 1, Label = "CASUAL",
                System =
                    "You rewrite text in a relaxed, friendly register — plain words, " +
                    "contractions, no corporate phrasing. " + NoInvention + " " +
                    "Keep the result within roughly 15% of the original length. " +
                    "Preserve line breaks and list structure. " + NoPreamble,
                Temperature = 0.4,
                Examples =
                {
                    new StyleExample
                    {
                        In  = "I am writing to enquire whether the submission deadline has been revised.",
                        Out = "Just wanted to check whether the deadline moved?",
                    },
                },
            },
            new()
            {
                Key = 2, Label = "EMAIL",
                Structured = true,
                // "Use only facts present in the input" (NoInvention) is wrong
                // here and is deliberately not used: the model read it as "add
                // no words" and merely wrapped the input in a greeting and a
                // sign-off. Expanding the *language* and inventing *facts* are
                // different things, so they are stated separately — and the
                // input is framed as intent rather than as body text, or prose
                // input gets decorated instead of rewritten.
                System =
                    "You write complete emails from the user's input. Treat that input as a " +
                    "description of what the sender wants to say — never as the text of the " +
                    "email itself. Rewrite it completely into a proper message, even when the " +
                    "input is already made of full sentences: add the framing, connective " +
                    "phrasing and courtesy an email needs, and organise it into paragraphs. " +
                    "Never simply wrap the input in a greeting and a sign-off. " +
                    "Do not invent specific facts. Names, dates, times, numbers and events must " +
                    "come from the input; where one is needed but missing, insert a bracketed " +
                    "placeholder such as [DATE] or [COURSE CODE] rather than guessing. " +
                    "Write a greeting, a body and a sign-off. " +
                    "Do not include a subject line in the body — it is returned separately. " +
                    "Do not add a signature block; the mail client appends one. " +
                    "Register: professional and courteous, suitable for writing to a " +
                    "professor or someone senior. Keep it to three short paragraphs at most.",
                Examples =
                {
                    new StyleExample
                    {
                        In  = "ask prof for extension, was sick last week, need 3 more days for lab report",
                        Out =
                            "{\"subject\":\"Request for extension \\u2014 lab report\"," +
                            "\"body\":\"Dear Professor [NAME],\\n\\n" +
                            "I am writing to request a short extension for the [COURSE] lab report. " +
                            "I was unwell last week and was unable to complete the work in time.\\n\\n" +
                            "If possible, I would be grateful for three additional days, which would " +
                            "move my submission to [DATE]. Please let me know if that is acceptable.\\n\\n" +
                            "Thank you for your understanding.\\n\\nBest regards\"}",
                    },
                },
            },
            new()
            {
                Key = 3, Label = "SHORTER",
                System =
                    "You cut text down. Remove filler, hedging, and repetition while keeping " +
                    "every fact and the author's voice. " + NoInvention + " " +
                    "Aim for roughly half the original length. " + NoPreamble,
                Temperature = 0.2,
            },
            new()
            {
                Key = 4, Label = "CLEARER",
                System =
                    "You rewrite text for clarity. Untangle long sentences, fix ambiguous " +
                    "references, and put the main point first. Keep the author's register — " +
                    "do not make it more formal. " + NoInvention + " " + NoPreamble,
            },
            new()
            {
                Key = 6, Label = "ASK",
                // The odd one out: every other style transforms the selection,
                // this one *executes* it. Deliberately omits NoInvention —
                // producing new content is the entire point, and the shared
                // "use only facts present in the input" rule would gut it.
                //
                // Accepting replaces the question with the answer, which is
                // what you want when the question was a scratch line you typed
                // in place. The preview is editable if it isn't.
                System =
                    "Answer the user's request directly. If they ask for text, produce that " +
                    "text. If they ask a question, answer it. Be concise — a few sentences or " +
                    "a short list unless the request clearly needs more. Do not restate the " +
                    "request, do not explain your reasoning, and do not add pleasantries. " +
                    NoPreamble,
            },
            new()
            {
                Key = 7, Label = "SUMMARY",
                // Thinking on: like proofreading, condensing faithfully means
                // weighing the whole input before writing, and that is what the
                // reasoning pass buys. Costs roughly 15-20 s.
                Think = true,
                Temperature = 0.2,
                System =
                    "Summarise the user's text. Capture the main points and any decisions, " +
                    "numbers, names or dates that matter. Use a short paragraph for brief text " +
                    "and three to five bullet points for anything longer. Be substantially " +
                    "shorter than the original. " + NoInvention + " " + NoPreamble,
            },
            new()
            {
                Key = 5, Label = "FIX",
                // Leads with the action. An earlier version opened with the
                // restrictions ("change nothing else… leave it byte-for-byte
                // unchanged") and the model reliably echoed the input back
                // untouched at every temperature — the prohibition drowned out
                // the instruction. State the job first, constrain second.
                System =
                    "Correct every spelling mistake, typo, grammatical error, and punctuation " +
                    "error in the user's text. Fix capitalisation — including the pronoun \"I\" " +
                    "and the first word of every sentence — and add missing sentence-ending " +
                    "punctuation. Keep the author's exact wording, word order, " +
                    "tone and line breaks: do not rephrase, reorder, merge or split sentences, " +
                    "and do not make the text more formal. " + NoInvention + " " + NoPreamble,
                Temperature = 0.0,
                Think = true,
            },
        },
    };

    public void Dispose() { _fsw.Dispose(); _debounce?.Dispose(); }
}
