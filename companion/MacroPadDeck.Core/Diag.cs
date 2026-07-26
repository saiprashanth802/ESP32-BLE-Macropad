using System.IO;

namespace MacroPadDeck;

/// Shared diagnostic sink. Every module — core and both platform front-ends —
/// logs to the same deck.log next to profiles.json, so a single file tells the
/// whole story regardless of which layer produced the line.
public static class Diag
{
    static readonly string LogPath = Path.Combine(ProfileStore.Dir, "deck.log");

    public static void Log(string msg)
    {
        try { File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} {msg}{Environment.NewLine}"); }
        catch { /* logging must never throw into the caller */ }
    }
}
