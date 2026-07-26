using Tmds.DBus;

namespace MacroPadDeck;

/// Linux now-playing source: reads the desktop's MPRIS players over the session
/// D-Bus and hands a normalised TrackInfo to Core's MediaPusher (the SMTC
/// counterpart on Windows). Covers Spotify, browsers, and most native players —
/// anything that exposes org.mpris.MediaPlayer2.
public sealed class MprisMediaSource : IMediaSource
{
    const string MprisPrefix = "org.mpris.MediaPlayer2.";
    const string PlayerPath = "/org/mpris/MediaPlayer2";

    static void Log(string m) => Diag.Log($"mpris: {m}");
    static string _lastSkip = "";      // dedupe per-player read failures (1 Hz poll)

    public async Task<MediaPoll> Poll()
    {
        try
        {
            var conn = Connection.Session;
            var names = await conn.ListServicesAsync();
            var players = names.Where(n => n.StartsWith(MprisPrefix, StringComparison.Ordinal)).ToArray();
            if (players.Length == 0) return MediaPoll.Idle;

            // Prefer a player that is actually Playing; otherwise take the first
            // that carries a title, so a paused/backgrounded player still shows.
            TrackInfo? firstWithTitle = null;
            foreach (var name in players)
            {
                var t = await ReadPlayer(conn, name);
                if (t is null) continue;
                if (t.Value.Playing) return MediaPoll.Of(t.Value);
                firstWithTitle ??= t;
            }
            return firstWithTitle is not null ? MediaPoll.Of(firstWithTitle.Value) : MediaPoll.Idle;
        }
        catch (Exception ex)
        {
            // A player vanishing mid-poll throws; report Unknown so the pad isn't
            // cleared on a transient fault, matching the Windows SMTC behaviour.
            Log($"EX {ex.GetType().Name} — {ex.Message.Split('\n')[0]}");
            return MediaPoll.Unknown;
        }
    }

    static async Task<TrackInfo?> ReadPlayer(Connection conn, string name)
    {
        string sn = name.Length > MprisPrefix.Length ? name[MprisPrefix.Length..] : name;
        try
        {
            var player = conn.CreateProxy<IMprisPlayer>(name, PlayerPath);
            var all = await player.GetAllAsync();

            string status = all.TryGetValue("PlaybackStatus", out var st) ? st as string ?? "" : "";
            if (status == "Stopped") return null;
            bool playing = status == "Playing";

            if (!all.TryGetValue("Metadata", out var mObj) || mObj is not IDictionary<string, object> meta)
                return null;

            string title = meta.TryGetValue("xesam:title", out var ti) ? ti as string ?? "" : "";
            if (meta.TryGetValue("xesam:artist", out var ar) && ar is string[] artists && artists.Length > 0
                && artists[0].Length > 0 && title.Length > 0)
                title = $"{title} - {artists[0]}";
            if (title.Length == 0) return null;

            // MPRIS times are microseconds.
            int dur = meta.TryGetValue("mpris:length", out var ln) ? (int)(ToLong(ln) / 1_000_000) : 0;
            int pos = all.TryGetValue("Position", out var ps) ? (int)(ToLong(ps) / 1_000_000) : 0;
            if (dur > 0) pos = Math.Min(pos, dur);

            return new TrackInfo(title, playing, pos, dur, sn);
        }
        catch (Exception ex)
        {
            // Skip a misbehaving player — but surface *why* once (deduped), because
            // a silently swallowed read here previously masked a total MPRIS outage.
            string sig = $"{sn}:{ex.GetType().Name}";
            if (sig != _lastSkip) { _lastSkip = sig; Log($"read '{sn}' skipped — {ex.GetType().Name}: {ex.Message.Split('\n')[0]}"); }
            return null;
        }
    }

    static long ToLong(object o) => o switch
    {
        long l => l,
        ulong u => (long)u,
        int i => i,
        uint u => u,
        _ => Convert.ToInt64(o),
    };
}

/// The MPRIS Player interface. Tmds.DBus maps the parameterless GetAllAsync() to
/// org.freedesktop.DBus.Properties.GetAll for this interface name, returning
/// PlaybackStatus, Metadata and Position in one call. A generic
/// org.freedesktop.DBus.Properties proxy that takes an interface-name argument is
/// NOT supported by Tmds.DBus 0.94 — its property accessors must be parameterless.
[DBusInterface("org.mpris.MediaPlayer2.Player")]
public interface IMprisPlayer : IDBusObject
{
    Task<IDictionary<string, object>> GetAllAsync();
}
