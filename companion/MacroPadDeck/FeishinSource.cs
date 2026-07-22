using System.IO;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;

namespace MacroPadDeck;

/// Now-playing from Feishin's Remote server. Feishin never publishes to
/// Windows media sessions, so this is the only way to see its playback.
///
/// The remote is a WebSocket, not REST (every /api/* path 404s): connect to
/// ws://host:port/ with HTTP Basic auth and receive {"event":…,"data":…}
/// frames. Events seen in remote.js: state (full snapshot on connect), song,
/// position, playback, volume, repeat, shuffle, favorite, rating, proxy.
///
/// Credentials come from profiles.json and are used only for this handshake.
public sealed class FeishinSource : IDisposable
{
    readonly Uri _uri;
    readonly string _basic;
    readonly CancellationTokenSource _cts = new();

    // Snapshot updated by the receive loop, read by MediaWatcher.
    public volatile string Title = "";
    public volatile bool Playing;
    volatile string _songId = "";
    volatile bool _userFavorite;
    ClientWebSocket? _ws;                 // live socket, for outbound control
    volatile int _pos, _dur;
    DateTime _posAt = DateTime.MinValue;
    public DateTime LastUpdate { get; private set; } = DateTime.MinValue;

    public int Duration => _dur;
    public bool IsFavorite => _userFavorite;
    /// Song name without the " - artist" suffix, for matching against SMTC.
    public string SongName => Title.Split(" - ")[0];
    /// Position extrapolated from the last update, so the pad's bar keeps
    /// moving between Feishin's periodic position events.
    public int Position => Playing && _posAt > DateTime.MinValue
        ? _pos + (int)(DateTime.UtcNow - _posAt).TotalSeconds
        : _pos;

    public FeishinSource(string baseUrl, string user, string password)
    {
        var http = new Uri(baseUrl);
        _uri = new Uri((http.Scheme == "https" ? "wss://" : "ws://") + http.Authority + "/");
        _basic = Convert.ToBase64String(Encoding.UTF8.GetBytes($"{user}:{password}"));
        _ = Task.Run(RunLoop);
    }

    static void Log(string m)
    {
        try { File.AppendAllText(Path.Combine(ProfileStore.Dir, "deck.log"),
                                 $"{DateTime.Now:HH:mm:ss.fff} feishin: {m}\r\n"); } catch { }
    }

    async Task RunLoop()
    {
        bool loggedFail = false;
        while (!_cts.IsCancellationRequested)
        {
            try
            {
                using var ws = new ClientWebSocket();
                ws.Options.SetRequestHeader("Authorization", "Basic " + _basic);
                ws.Options.KeepAliveInterval = TimeSpan.FromSeconds(15);   // server drops idle sockets
                await ws.ConnectAsync(_uri, _cts.Token);
                Log($"connected {_uri}");
                loggedFail = false;
                _ws = ws;
                // Feishin's own client authenticates right after connecting;
                // without it we're a passive listener that receives the initial
                // state and then no live `song` updates — which silently leaves
                // the cached track stale (and favorites would hit the wrong song).
                await SendAuthenticate(ws);
                try { await Receive(ws); } finally { _ws = null; }
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex)
            {
                if (!loggedFail) { loggedFail = true; Log($"connect failed: {ex.Message.Split('\r')[0]}"); }
            }
            // Keep the last known track across a reconnect — the server closes
            // idle sockets routinely and blanking the pad each time flickers.
            // MediaWatcher's staleness check retires it if we stay down.
            try { await Task.Delay(1500, _cts.Token); } catch { return; }
        }
    }

    /// {"event":"authenticate","header":<value from GET /credentials>} — same
    /// handshake remote.js performs. Best-effort: if /credentials refuses we
    /// still listen, we just may not get live updates.
    async Task SendAuthenticate(ClientWebSocket ws)
    {
        try
        {
            using var http = new System.Net.Http.HttpClient { Timeout = TimeSpan.FromSeconds(5) };
            http.DefaultRequestHeaders.Add("Authorization", "Basic " + _basic);
            var resp = await http.GetAsync(_uri.ToString().Replace("ws://", "http://")
                                                          .Replace("wss://", "https://") + "credentials");
            if (!resp.IsSuccessStatusCode) { Log($"credentials: {(int)resp.StatusCode} — check feishinPassword"); return; }
            string header = (await resp.Content.ReadAsStringAsync()).Trim().Trim('"');
            string json = JsonSerializer.Serialize(new { @event = "authenticate", header });
            await ws.SendAsync(Encoding.UTF8.GetBytes(json), WebSocketMessageType.Text, true, _cts.Token);
            Log("authenticated");
        }
        catch (Exception ex) { Log($"authenticate failed: {ex.Message.Split('\r')[0]}"); }
    }

    async Task Receive(ClientWebSocket ws)
    {
        var buf = new byte[16 * 1024];
        var sb = new StringBuilder();
        while (ws.State == WebSocketState.Open && !_cts.IsCancellationRequested)
        {
            var r = await ws.ReceiveAsync(buf, _cts.Token);
            if (r.MessageType == WebSocketMessageType.Close) return;
            sb.Append(Encoding.UTF8.GetString(buf, 0, r.Count));
            if (!r.EndOfMessage) continue;          // frames can span reads
            string msg = sb.ToString();
            sb.Clear();
            try { Handle(msg); } catch { /* unexpected shape — ignore this frame */ }
        }
    }

    void Handle(string json)
    {
        using var doc = JsonDocument.Parse(json);
        if (!doc.RootElement.TryGetProperty("event", out var ev)) return;
        var data = doc.RootElement.TryGetProperty("data", out var d) ? d : default;

        switch (ev.GetString())
        {
            case "state":                            // full snapshot on connect
                if (data.ValueKind == JsonValueKind.Object)
                {
                    if (data.TryGetProperty("song", out var sng)) ReadSong(sng);
                    if (data.TryGetProperty("position", out var p)) SetPos(Num(p));
                    if (data.TryGetProperty("status", out var st)) SetStatus(st);
                }
                break;
            case "song":     ReadSong(data); break;
            case "position": SetPos(Num(data)); break;
            case "playback": SetStatus(data); break;
            case "favorite":                        // server echo — keep in sync
                if (data.ValueKind == JsonValueKind.Object &&
                    data.TryGetProperty("id", out var fid) && fid.GetString() == _songId)
                    _userFavorite = data.TryGetProperty("favorite", out var fv) &&
                                    fv.ValueKind == JsonValueKind.True;
                break;
        }
        LastUpdate = DateTime.UtcNow;
    }

    void ReadSong(JsonElement s)
    {
        if (s.ValueKind != JsonValueKind.Object) { Title = ""; _songId = ""; return; }
        string name = s.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "";
        string artist = s.TryGetProperty("artistName", out var a) ? a.GetString() ?? "" : "";
        // ASCII only — the pad's 5x7 font renders anything else as '?'
        Title = artist.Length > 0 && name.Length > 0 ? $"{name} - {artist}" : name;
        if (s.TryGetProperty("duration", out var du)) _dur = Norm(Num(du));
        _songId = s.TryGetProperty("id", out var id) ? id.GetString() ?? "" : "";
        _userFavorite = s.TryGetProperty("userFavorite", out var uf) &&
                        uf.ValueKind == JsonValueKind.True;
        LastUpdate = DateTime.UtcNow;
    }

    /// Toggle the favorite flag on whatever is playing.
    /// Wire format taken from remote.js: {"event":"favorite","favorite":<bool>,"id":<songId>}.
    /// Feishin echoes a `favorite` event back, which updates our cached state,
    /// so repeated presses alternate correctly.
    /// `displayedTitle` is what the pad is currently showing. Favoriting is
    /// refused unless Feishin agrees it's the same track — a stale cached song
    /// once caused the wrong track to be favorited, and silently editing the
    /// user's library is far worse than doing nothing.
    public async Task<(bool ok, bool nowFavorite, string title, string error)>
        ToggleFavorite(string displayedTitle)
    {
        // The socket is dropped and re-established routinely; wait briefly
        // rather than failing a press that landed during a reconnect.
        var ws = _ws;
        for (int i = 0; i < 12 && (ws is null || ws.State != WebSocketState.Open); i++)
        {
            await Task.Delay(250, _cts.Token);
            ws = _ws;
        }
        if (ws is null || ws.State != WebSocketState.Open)
        { Log("favorite: no socket"); return (false, false, "", "NO FEISHIN LINK"); }
        if (_songId.Length == 0)
        { Log("favorite: no song"); return (false, false, "", "NOTHING PLAYING"); }

        if (displayedTitle.Length > 0)
        {
            string mine = SongName.Trim();
            if (mine.Length > 0 &&
                !displayedTitle.StartsWith(mine, StringComparison.OrdinalIgnoreCase) &&
                !displayedTitle.Contains(mine, StringComparison.OrdinalIgnoreCase))
            {
                Log($"favorite: REFUSED — pad shows '{displayedTitle}', feishin has '{mine}'");
                return (false, false, "", "TRACK MISMATCH");
            }
        }
        bool target = !_userFavorite;
        string json = JsonSerializer.Serialize(new
        {
            @event = "favorite",
            favorite = target,
            id = _songId,
        });
        try
        {
            await ws.SendAsync(Encoding.UTF8.GetBytes(json), WebSocketMessageType.Text,
                               true, _cts.Token);
            _userFavorite = target;         // optimistic; the echo confirms
            Log($"favorite: {(target ? "set" : "cleared")} on '{Title}'");
            return (true, target, Title, "");
        }
        catch (Exception ex)
        {
            Log($"favorite failed: {ex.Message.Split('\r')[0]}");
            return (false, false, "", "FAVORITE FAILED");
        }
    }

    void SetPos(double v) { _pos = Norm(v); _posAt = DateTime.UtcNow; }

    void SetStatus(JsonElement st)
    {
        Playing = st.ValueKind switch
        {
            JsonValueKind.String => (st.GetString() ?? "").ToLowerInvariant() is "playing" or "play",
            JsonValueKind.Number => st.GetDouble() != 0,
            JsonValueKind.True => true,
            _ => false,
        };
        _pos = Position;                 // re-base extrapolation on state change
        _posAt = DateTime.UtcNow;
    }

    static double Num(JsonElement e) => e.ValueKind == JsonValueKind.Number ? e.GetDouble() : 0;
    /// Feishin reports milliseconds in some builds, seconds in others.
    static int Norm(double v) => (int)(v > 10000 ? v / 1000 : v);

    public void Dispose() { _cts.Cancel(); _cts.Dispose(); }
}
