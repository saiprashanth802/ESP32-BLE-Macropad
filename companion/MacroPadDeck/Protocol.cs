namespace MacroPadDeck;

/// Host-link wire protocol. TLV both directions: [opcode:1][len:1][payload].
/// Mirror of the firmware tables in macropad_v5.ino / docs/CONFIG_API.md.
public static class Protocol
{
    public static readonly Guid Service = Guid.Parse("6d616372-6f70-6164-0000-000000000001");
    public static readonly Guid EvtChar = Guid.Parse("6d616372-6f70-6164-0000-000000000002");
    public static readonly Guid CmdChar = Guid.Parse("6d616372-6f70-6164-0000-000000000003");

    // device → host
    public const byte EvHello  = 0x01;
    public const byte EvKey    = 0x02;
    public const byte EvPreset = 0x03;

    // host → device
    public const byte CmdLabel  = 0x81;
    public const byte CmdStatus = 0x82;
    public const byte CmdPreset = 0x83;
    public const byte CmdFace   = 0x84;

    public static byte[] SetLabel(int preset, int key, string label)
    {
        byte[] txt = System.Text.Encoding.ASCII.GetBytes(Sanitize(label, 8));
        byte[] b = new byte[4 + txt.Length];
        b[0] = CmdLabel; b[1] = (byte)(2 + txt.Length);
        b[2] = (byte)preset; b[3] = (byte)key;
        txt.CopyTo(b, 4);
        return b;
    }

    public static byte[] SetStatus(string text)
    {
        byte[] txt = System.Text.Encoding.ASCII.GetBytes(Sanitize(text, 23));
        byte[] b = new byte[2 + txt.Length];
        b[0] = CmdStatus; b[1] = (byte)txt.Length;
        txt.CopyTo(b, 2);
        return b;
    }

    public static byte[] SetPreset(int preset) => new byte[] { CmdPreset, 1, (byte)preset };

    // The pad's 5x7 font is ASCII-only; strip anything else and cap length.
    static string Sanitize(string s, int max)
    {
        var sb = new System.Text.StringBuilder(max);
        foreach (char c in s)
        {
            if (sb.Length >= max) break;
            sb.Append(c is >= ' ' and <= '~' ? c : '?');
        }
        return sb.ToString();
    }
}
