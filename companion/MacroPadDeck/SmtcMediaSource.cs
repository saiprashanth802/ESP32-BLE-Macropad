using Windows.Media.Control;

namespace MacroPadDeck;

/// Windows now-playing source: reads the system-wide SMTC session and hands a
/// normalised TrackInfo to Core's MediaPusher, which owns the change-detection,
/// heartbeat and Feishin merge. Covers every SMTC source — Feishin, Spotify,
/// browsers — with one API.
public sealed class SmtcMediaSource : IMediaSource
{
    GlobalSystemMediaTransportControlsSessionManager? _mgr;
    int _emptyPolls;               // consecutive empty polls — drives recycling

    static void Log(string m) => Diag.Log($"smtc: {m}");

    public async Task<MediaPoll> Poll()
    {
        try
        {
            _mgr ??= await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            // GetCurrentSession() is null unless Windows has designated a
            // "current" app; fall back to any session that is actually playing,
            // then to the first one, so background players still register.
            var s = _mgr.GetCurrentSession();
            if (s is null)
            {
                var all = _mgr.GetSessions();
                s = all.FirstOrDefault(x => x.GetPlaybackInfo().PlaybackStatus ==
                        GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
                    ?? all.FirstOrDefault();
                if (s is null)
                {
                    // A manager reporting zero sessions is usually stale rather
                    // than genuinely idle; recycle it every few empty polls.
                    if (++_emptyPolls % 4 == 0) _mgr = null;
                    return MediaPoll.Idle;
                }
                _emptyPolls = 0;
                Log($"using fallback session: {s.SourceAppUserModelId}");
            }

            var props = await s.TryGetMediaPropertiesAsync();
            var tl = s.GetTimelineProperties();
            bool playing = s.GetPlaybackInfo().PlaybackStatus ==
                           GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;
            string title = props?.Title ?? "";
            if (title.Length == 0) return MediaPoll.Idle;

            int dur = (int)(tl.EndTime - tl.StartTime).TotalSeconds;

            // SMTC's Position is a SNAPSHOT taken at LastUpdatedTime, not a live
            // clock — it only moves when the player pushes an update. Age it here
            // so what we send is the real current position (otherwise the pad's
            // own extrapolation creeps forward then snaps back on the next poll).
            int pos = (int)tl.Position.TotalSeconds;
            if (playing)
            {
                double age = (DateTimeOffset.UtcNow - tl.LastUpdatedTime).TotalSeconds;
                if (age > 0 && age < 3600) pos += (int)age;      // ignore absurd clocks
            }
            if (dur > 0) pos = Math.Min(pos, dur);

            return MediaPoll.Of(new TrackInfo(title, playing, pos, dur, s.SourceAppUserModelId ?? "smtc"));
        }
        catch (Exception ex)
        {
            // The SMTC manager and its session objects go stale whenever the set
            // of sessions changes (track end, app focus shift) — the proxy then
            // throws and reports zero sessions forever. Drop it so the next tick
            // requests a fresh one; report Unknown so the pad isn't cleared on a
            // transient fault.
            _mgr = null;
            Log($"EX {ex.GetType().Name} 0x{ex.HResult:X8} — manager reset");
            return MediaPoll.Unknown;
        }
    }
}
