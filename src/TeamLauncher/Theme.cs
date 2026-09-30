using System.Drawing.Drawing2D;
using System.Drawing.Text;

namespace TeamLauncher;

/// <summary>
/// Thème minimaliste sombre neutre : fond quasi-noir, cartes gris très sombre,
/// un seul accent froid. Pas de bordures visibles, coins peu arrondis, typographie sobre.
/// </summary>
public static class Theme
{
    // Couleurs par défaut (surchargeables depuis Paramètres)
    private const string DefaultBg = "#0b0d10";
    private const string DefaultCard = "#15181d";
    private const string DefaultAccent = "#7aa2f7";

    // Anciennes valeurs du thème "Minecraft" à ignorer
    private static readonly HashSet<string> LegacyColors = new(StringComparer.OrdinalIgnoreCase)
        { "#141519", "#23262c", "#6fbf3f" };

    public static Color Bg { get; private set; }
    public static Color Card { get; private set; }
    public static Color Panel => ControlPaint.Dark(Card, 0.04f);
    public static Color Border => Color.FromArgb(28, 255, 255, 255); // séparateur très discret
    public static Color Accent { get; private set; }
    public static Color AccentHover => ControlPaint.Light(Accent, 0.10f);
    public static Color AccentDim => ControlPaint.Dark(Accent, 0.45f);
    public static Color Hover => ControlPaint.Light(Card, 0.04f);
    public static Color Text { get; private set; } = FromHex("#e6e8eb");
    public static Color TextDim { get; private set; } = FromHex("#8b919a");

    private static readonly Font _titleFont = new("Segoe UI", 13f, FontStyle.Regular);
    public static Font Title => _titleFont;

    static Theme() => Reload();

    public static void Reload()
    {
        var s = DataStore.Settings;
        Bg = ParseOr(Clean(s.BgColor), DefaultBg);
        Card = ParseOr(Clean(s.CardColor), DefaultCard);
        Accent = ParseOr(Clean(s.AccentColor), DefaultAccent);
        LoadBackground();
    }

    private static string Clean(string? value) =>
        string.IsNullOrWhiteSpace(value) || LegacyColors.Contains(value.Trim()) ? "" : value.Trim();

    // ---- image de fond personnalisée ----
    private static Image? bgImage;
    private static MemoryStream? bgStream;

    public static Image? BgImage => bgImage;
    public static bool HasBgImage => bgImage != null;

    private static void LoadBackground()
    {
        bgImage?.Dispose();
        bgImage = null;
        bgStream?.Dispose();
        bgStream = null;

        string p = DataStore.Settings.BackgroundImagePath ?? "";
        if (p.Length == 0 || !File.Exists(p)) return;
        try
        {
            using var original = Image.FromFile(p);
            // Redimensionner si l'image dépasse la résolution de l'écran pour libérer de la RAM
            var screen = Screen.PrimaryScreen?.Bounds ?? new Rectangle(0, 0, 1920, 1080);
            int maxW = screen.Width, maxH = screen.Height;
            if (original.Width > maxW || original.Height > maxH)
            {
                float scale = Math.Min((float)maxW / original.Width, (float)maxH / original.Height);
                int w = (int)(original.Width * scale), h = (int)(original.Height * scale);
                var resized = new Bitmap(w, h);
                using (var g = Graphics.FromImage(resized))
                {
                    g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                    g.DrawImage(original, 0, 0, w, h);
                }
                bgStream = new MemoryStream();
                resized.Save(bgStream, System.Drawing.Imaging.ImageFormat.Png);
                bgStream.Position = 0;
                bgImage = Image.FromStream(bgStream);
                resized.Dispose();
            }
            else
            {
                bgStream = new MemoryStream(File.ReadAllBytes(p));
                bgImage = Image.FromStream(bgStream);
            }
        }
        catch { bgImage = null; }
    }

    public static void SetBackground(string sourcePath)
    {
        string dest = Path.Combine(DataStore.ImagesDir, "background" + Path.GetExtension(sourcePath).ToLowerInvariant());
        Directory.CreateDirectory(DataStore.ImagesDir);
        File.Copy(sourcePath, dest, overwrite: true);
        DataStore.Settings.BackgroundImagePath = dest;
        DataStore.Save();
        Reload();
    }

    public static void ClearBackground()
    {
        DataStore.Settings.BackgroundImagePath = "";
        DataStore.Save();
        try
        {
            foreach (var f in Directory.GetFiles(DataStore.ImagesDir, "background.*"))
                File.Delete(f);
        }
        catch { }
        Reload();
    }

    public static void Save(string bg, string card, string accent)
    {
        var s = DataStore.Settings;
        s.BgColor = bg; s.CardColor = card; s.AccentColor = accent;
        DataStore.Save();
        Reload();
    }

    // ---- style des contrôles ----

    /// <summary>Carte minimaliste : coins légèrement arrondis, pas de bordure dessinée.</summary>
    public static void Blockify(Control c) => Round(c, 6);

    /// <summary>Coins arrondis (réappliqués au redimensionnement).</summary>
    public static void Round(Control c, int radius)
    {
        int lastW = 0, lastH = 0;
        void Apply()
        {
            if (c.Width <= 0 || c.Height <= 0) return;
            if (c.Width == lastW && c.Height == lastH) return;
            lastW = c.Width; lastH = c.Height;
            int r = Math.Min(radius, Math.Min(c.Width, c.Height) / 2);
            var path = new System.Drawing.Drawing2D.GraphicsPath();
            path.AddArc(0, 0, r, r, 180, 90);
            path.AddArc(c.Width - r - 1, 0, r, r, 270, 90);
            path.AddArc(c.Width - r - 1, c.Height - r - 1, r, r, 0, 90);
            path.AddArc(0, c.Height - r - 1, r, r, 90, 90);
            path.CloseFigure();
            c.Region?.Dispose();
            var region = new Region(path);
            path.Dispose();
            c.Region = region;
        }
        c.Resize += (_, _) => Apply();
        Apply();
    }

    public static void Apply(Button b, bool primary = false)
    {
        b.FlatStyle = FlatStyle.Flat;
        b.FlatAppearance.BorderSize = 0;
        b.FlatAppearance.MouseOverBackColor = primary ? AccentHover : Hover;
        b.BackColor = primary ? Accent : ControlPaint.Light(Card, 0.04f);
        b.ForeColor = primary ? OnColor(Accent) : Text;
        b.Cursor = Cursors.Hand;
        b.Font = new Font("Segoe UI", 9f);
        b.Height = Math.Min(Math.Max(b.Height, 28), 34);
        Round(b, 5);
    }

    /// <summary>Applique le style sombre à un TextBox.</summary>
    public static void ApplyInput(TextBox t)
    {
        t.BackColor = Card;
        t.ForeColor = Text;
        t.BorderStyle = BorderStyle.None;
        t.Font = new Font("Segoe UI", 9.5f);
        t.Padding = new Padding(4);
    }

    /// <summary>Applique le style sombre à un ComboBox.</summary>
    public static void ApplyInput(ComboBox c)
    {
        c.BackColor = Card;
        c.ForeColor = Text;
        c.FlatStyle = FlatStyle.Flat;
        c.Font = new Font("Segoe UI", 9.5f);
    }

    /// <summary>Applique le style sombre à un NumericUpDown.</summary>
    public static void ApplyInput(NumericUpDown n)
    {
        n.BackColor = Card;
        n.ForeColor = Text;
        n.Font = new Font("Segoe UI", 9.5f);
    }

    /// <summary>Applique le style sombre à un TabControl (supprime les bordures blanches).</summary>
    public static void ApplyTab(TabControl t)
    {
        t.Appearance = TabAppearance.FlatButtons;
        t.BackColor = Bg;
        t.ForeColor = Text;
        t.DrawMode = TabDrawMode.OwnerDrawFixed;
        t.BorderStyle = BorderStyle.None;

        // Supprimer la bordure blanche native Win32
        t.HandleCreated += (_, _) =>
        {
            const int GWL_STYLE = -16;
            const int WS_BORDER = 0x00800000;
            const int WS_EX_CLIENTEDGE = 0x00000200;
            nint hwnd = t.Handle;
            nint style = GetWindowLong(hwnd, GWL_STYLE);
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_BORDER);
            nint exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
            SetWindowLong(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_CLIENTEDGE);
            SetWindowPos(hwnd, IntPtr.Zero, 0, 0, 0, 0,
                0x0001 | 0x0002 | 0x0020); // SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED
        };
    }

    private static Color OnColor(Color bg) =>
        (bg.R * 299 + bg.G * 587 + bg.B * 114) / 1000 > 150
            ? Color.FromArgb(18, 20, 16)
            : Color.White;

    // ---------------- helpers couleurs ----------------

    private static Color ParseOr(string? value, string fallback) =>
        !string.IsNullOrWhiteSpace(value) && TryParse(value.Trim(), out var c) ? c : FromHex(fallback);

    private static bool TryParse(string hex, out Color color)
    {
        try
        {
            if (hex.StartsWith('#')) hex = hex[1..];
            color = Color.FromArgb(
                Convert.ToInt32(hex[..2], 16),
                Convert.ToInt32(hex.Substring(2, 2), 16),
                Convert.ToInt32(hex.Substring(4, 2), 16));
            return true;
        }
        catch { color = default; return false; }
    }

    private static Color FromHex(string hex) { TryParse(hex, out var c); return c; }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern nint GetWindowLong(IntPtr hWnd, int nIndex);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern nint SetWindowLong(IntPtr hWnd, int nIndex, nint dwNewLong);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int X, int Y, int cx, int cy, uint uFlags);
}
