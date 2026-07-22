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
    public const byte CmdColor  = 0x85;
    public const byte CmdKey    = 0x86;
    public const byte CmdCommit = 0x87;
    public const byte CmdText   = 0x88;
    public const byte CmdEyes   = 0x89;

    // firmware KAType values
    public const byte KaBuiltin = 0, KaKey = 1, KaConsumer = 2, KaText = 4, KaHost = 5;

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

    /// "#RRGGBB" → RGB565 accent + eye color for one preset. Null on bad input.
    public static byte[]? SetColor(int preset, string hex)
    {
        if (string.IsNullOrWhiteSpace(hex)) return null;
        hex = hex.TrimStart('#');
        if (hex.Length != 6 || !uint.TryParse(hex, System.Globalization.NumberStyles.HexNumber, null, out uint rgb))
            return null;
        int r = (int)(rgb >> 16) & 0xFF, g = (int)(rgb >> 8) & 0xFF, b = (int)rgb & 0xFF;
        ushort c565 = (ushort)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        return new byte[] { CmdColor, 3, (byte)preset, (byte)(c565 >> 8), (byte)(c565 & 0xFF) };
    }

    /// Rewrite one pad key: [preset][key][kaType][mod][hid][cons lo][cons hi][label…]
    public static byte[] SetKey(int preset, int key, byte kaType, byte mod, byte hid,
                                ushort consumer, string label)
    {
        byte[] txt = System.Text.Encoding.ASCII.GetBytes(Sanitize(label, 8));
        byte[] b = new byte[9 + txt.Length];
        b[0] = CmdKey; b[1] = (byte)(7 + txt.Length);
        b[2] = (byte)preset; b[3] = (byte)key; b[4] = kaType;
        b[5] = mod; b[6] = hid;
        b[7] = (byte)(consumer & 0xFF); b[8] = (byte)(consumer >> 8);
        txt.CopyTo(b, 9);
        return b;
    }

    public static byte[] SetText(int preset, int key, string text)
    {
        byte[] txt = System.Text.Encoding.ASCII.GetBytes(Sanitize(text, 23));
        byte[] b = new byte[4 + txt.Length];
        b[0] = CmdText; b[1] = (byte)(2 + txt.Length);
        b[2] = (byte)preset; b[3] = (byte)key;
        txt.CopyTo(b, 4);
        return b;
    }

    public static byte[] Commit() => new byte[] { CmdCommit, 0 };

    /// Eye color: "#RRGGBB", or "preset" to follow the active preset's accent.
    public static byte[]? SetEyes(string spec, bool persist)
    {
        ushort c565;
        if (spec.Equals("preset", StringComparison.OrdinalIgnoreCase)) c565 = 0;
        else
        {
            string hex = spec.TrimStart('#');
            if (hex.Length != 6 || !uint.TryParse(hex, System.Globalization.NumberStyles.HexNumber, null, out uint rgb))
                return null;
            int r = (int)(rgb >> 16) & 0xFF, g = (int)(rgb >> 8) & 0xFF, b = (int)rgb & 0xFF;
            c565 = (ushort)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
        return new byte[] { CmdEyes, 3, (byte)(c565 >> 8), (byte)(c565 & 0xFF), (byte)(persist ? 1 : 0) };
    }

    /// Key names the editor offers → USB HID usage codes.
    public static readonly (string Name, byte Hid)[] HidKeys = BuildHidKeys();
    static (string, byte)[] BuildHidKeys()
    {
        var list = new List<(string, byte)>();
        for (char c = 'A'; c <= 'Z'; c++) list.Add((c.ToString(), (byte)(4 + c - 'A')));
        for (int d = 1; d <= 9; d++) list.Add((d.ToString(), (byte)(29 + d)));
        list.Add(("0", 39));
        list.AddRange(new (string, byte)[]
        {
            ("Enter", 40), ("Esc", 41), ("Backspace", 42), ("Tab", 43), ("Space", 44),
            ("Delete", 76), ("Home", 74), ("End", 77), ("PgUp", 75), ("PgDn", 78),
            ("Right", 79), ("Left", 80), ("Down", 81), ("Up", 82),
        });
        for (int f = 1; f <= 12; f++) list.Add(($"F{f}", (byte)(57 + f)));
        return list.ToArray();
    }

    /// Media actions → consumer usage codes.
    public static readonly (string Name, ushort Usage)[] MediaKeys =
    {
        ("Play / Pause", 205), ("Next track", 181), ("Previous track", 182),
        ("Mute", 226), ("Volume up", 233), ("Volume down", 234),
    };

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
