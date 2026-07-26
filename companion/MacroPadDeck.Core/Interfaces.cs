namespace MacroPadDeck;

/// The pad connection, abstracted away from any particular Bluetooth stack.
/// Windows implements this over WinRT, Linux over BlueZ/D-Bus. Everyone else
/// (DeckController, MediaPusher) sees only these plain events.
///
/// Disconnects are normal life (sleep, Easy-Switch, out of range) — an
/// implementation is expected to quietly re-acquire rather than surface errors.
public interface IBleLink : IDisposable
{
    bool IsUp { get; }
    event Action<byte, byte[]>? EventReceived;   // (opcode, payload)
    event Action<bool>? LinkChanged;             // subscribed / lost
    Task<bool> Write(byte[] cmd);
}

/// Runs a KeyBinding's host-side action (focus/launch an app, open a file,
/// run a command, snap a window). Every implementation must tolerate missing
/// targets — a bad binding must never crash the tray app.
public interface IActionEngine
{
    void Execute(KeyBinding b);
}

/// Raises the executable/app name whenever a different window takes the
/// foreground, driving profile auto-follow. Windows uses SetWinEventHook;
/// X11 polls _NET_ACTIVE_WINDOW. Wayland has no portable equivalent, so a
/// Wayland build supplies a no-op implementation.
public interface IForegroundWatcher : IDisposable
{
    event Action<string>? ExeChanged;
}

/// One snapshot of the system's now-playing state, normalised across sources
/// (Windows SMTC, Linux MPRIS). Position/duration are in whole seconds.
public readonly record struct TrackInfo(string Title, bool Playing, int PosSeconds, int DurSeconds, string SourceId);

public enum MediaState
{
    /// A real track is present (Track carries it).
    Track,
    /// Definitively nothing playing — clear the pad's strip.
    Idle,
    /// Transient error / stale provider — leave the pad as-is this tick.
    Unknown,
}

public readonly record struct MediaPoll(MediaState State, TrackInfo Track)
{
    public static readonly MediaPoll Idle = new(MediaState.Idle, default);
    public static readonly MediaPoll Unknown = new(MediaState.Unknown, default);
    public static MediaPoll Of(TrackInfo t) => new(MediaState.Track, t);
}

/// A platform's system-wide media source, polled by MediaPusher. Each call
/// returns the current state; the source owns its own connection lifecycle
/// (recycling a stale session manager, etc.) and never throws.
public interface IMediaSource
{
    Task<MediaPoll> Poll();
}

/// Exposes the track title the pad is currently displaying, so the favorite
/// action can guard against acting on a stale song. MediaPusher implements it.
public interface ICurrentTrack
{
    string CurrentTitle { get; }
}
