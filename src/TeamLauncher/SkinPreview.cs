using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace TeamLauncher;

public class SkinPreview : Control
{
    private Bitmap? _skin;
    private byte[]? _pixelData;
    private int _stride;
    private float _rotY = 0.5f;
    private float _rotX = 0.15f;
    private float _zoom = 1f;
    private float _walkPhase;
    private bool _walking;
    private bool _dragging;
    private Point _lastMouse;
    private readonly System.Windows.Forms.Timer _timer = new() { Interval = 100 };
    private readonly Pen _outlinePen;
    private readonly Font _infoFont;
    private readonly Dictionary<Color, SolidBrush> _brushCache = new();
    private Color[]? _faceColors;

    private struct FaceData
    {
        public PointF[] Pts;
        public Color Color;
        public float Depth;
    }
    private readonly List<FaceData> _faces = new(36);

    public bool Walking
    {
        get => _walking;
        set { _walking = value; _walkPhase = 0; if (_skin != null && !_timer.Enabled) StartTimer(); Invalidate(); }
    }

    public SkinPreview()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                 ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Card;
        MinimumSize = new Size(120, 160);
        _timer.Tick += OnTick;
        _outlinePen = new Pen(Color.Black, 0.7f);
        _infoFont = new Font("Consolas", 8f);
    }

    private void OnTick(object? sender, EventArgs e)
    {
        if (_skin == null) return;
        if (_walking) { _walkPhase += 0.12f; if (_walkPhase > MathF.PI * 2) _walkPhase -= MathF.PI * 2; }
        else { _rotY += 0.03f; }
        Invalidate();
    }

    protected override void OnVisibleChanged(EventArgs e)
    {
        base.OnVisibleChanged(e);
        if (Visible && _skin != null) StartTimer();
        else _timer.Stop();
    }

    private void StartTimer() { if (!_timer.Enabled && _skin != null) _timer.Start(); }

    public void SetSkin(Image? skin)
    {
        var old = _skin;
        _skin = skin == null ? null : new Bitmap(skin, new Size(64, 64));
        old?.Dispose();
        if (_skin != null)
        {
            ExtractPixels();
            PrecomputeFaceColors();
            _rotY = 0.5f;
            _walkPhase = 0;
            StartTimer();
        }
        else
        {
            _pixelData = null;
            _faceColors = null;
            _timer.Stop();
        }
        Invalidate();
    }

    private void ExtractPixels()
    {
        if (_skin == null) { _pixelData = null; return; }
        var rect = new Rectangle(0, 0, 64, 64);
        var bmpData = _skin.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        _stride = bmpData.Stride;
        _pixelData = new byte[_stride * 64];
        Marshal.Copy(bmpData.Scan0, _pixelData, 0, _pixelData.Length);
        _skin.UnlockBits(bmpData);
    }

    private Color SampleFast(RectangleF uv)
    {
        if (_pixelData == null) return Color.FromArgb(180, 160, 140);
        int x0 = Math.Clamp((int)uv.X, 0, 63), y0 = Math.Clamp((int)uv.Y, 0, 63);
        int x1 = Math.Clamp((int)uv.Right, 0, 63), y1 = Math.Clamp((int)uv.Bottom, 0, 63);
        long r = 0, gg = 0, b = 0, cnt = 0;
        for (int y = y0; y <= y1; y++)
        {
            int rowOff = y * _stride;
            for (int x = x0; x <= x1; x++)
            {
                int off = rowOff + x * 4;
                byte a = _pixelData[off + 3];
                if (a < 128) continue;
                r += _pixelData[off + 2];
                gg += _pixelData[off + 1];
                b += _pixelData[off];
                cnt++;
            }
        }
        if (cnt == 0) return Color.FromArgb(180, 160, 140);
        return Color.FromArgb(255, (int)(r / cnt), (int)(gg / cnt), (int)(b / cnt));
    }

    private static Color ShiftBrightness(Color c, float shift)
    {
        int r = Math.Clamp((int)(c.R * (1f + shift)), 0, 255);
        int g = Math.Clamp((int)(c.G * (1f + shift)), 0, 255);
        int b = Math.Clamp((int)(c.B * (1f + shift)), 0, 255);
        return Color.FromArgb(c.A, r, g, b);
    }

    private void PrecomputeFaceColors()
    {
        if (_pixelData == null) { _faceColors = null; return; }
        RectangleF[] uvs = [
            new(8, 8, 8, 8), new(24, 8, 8, 8), new(16, 8, 8, 8),
            new(0, 8, 8, 8), new(8, 0, 8, 8), new(16, 0, 8, 8),
            new(20, 20, 8, 12), new(32, 20, 8, 12), new(28, 20, 4, 12),
            new(16, 20, 4, 12), new(20, 16, 8, 4), new(28, 16, 8, 4),
            new(44, 20, 4, 12), new(52, 20, 4, 12), new(48, 20, 4, 12),
            new(40, 20, 4, 12), new(44, 16, 4, 4), new(48, 16, 4, 4),
            new(40, 52, 4, 12), new(48, 52, 4, 12), new(44, 52, 4, 12),
            new(36, 52, 4, 12), new(44, 48, 4, 4), new(48, 48, 4, 4),
            new(4, 20, 4, 12), new(12, 20, 4, 12), new(8, 20, 4, 12),
            new(0, 20, 4, 12), new(4, 16, 4, 4), new(8, 16, 4, 4),
            new(20, 52, 4, 12), new(28, 52, 4, 12), new(24, 52, 4, 12),
            new(16, 52, 4, 12), new(20, 48, 4, 4), new(24, 48, 4, 4)
        ];
        _faceColors = new Color[uvs.Length];
        for (int i = 0; i < uvs.Length; i++)
            _faceColors[i] = SampleFast(uvs[i]);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.InterpolationMode = InterpolationMode.NearestNeighbor;

        if (_skin == null || Width < 10 || Height < 10)
        {
            var sz = TextRenderer.MeasureText("Aucun skin", _infoFont);
            TextRenderer.DrawText(g, "Aucun skin", _infoFont,
                new Point((Width - sz.Width) / 2, (Height - sz.Height) / 2), Theme.TextDim);
            return;
        }

        using (var bg = new LinearGradientBrush(ClientRectangle,
            Color.FromArgb(40, 30, 50), Color.FromArgb(20, 20, 30), 90f))
            g.FillRectangle(bg, ClientRectangle);

        float scale = Math.Min(Width / 18f, Height / 34f) * _zoom;
        float cx = Width / 2f;
        float cy = Height * 0.48f;

        float armSwing = _walking ? MathF.Sin(_walkPhase) * 0.7f : 0f;
        float legSwing = _walking ? MathF.Sin(_walkPhase + MathF.PI) * 0.6f : 0f;
        float bodyBob = _walking ? MathF.Abs(MathF.Sin(_walkPhase * 2)) * 0.5f : 0f;

        float cosY = MathF.Cos(_rotY), sinY = MathF.Sin(_rotY);
        float cosX = MathF.Cos(_rotX), sinX = MathF.Sin(_rotX);

        PointF Project(float x, float y, float z)
        {
            float rx = x * cosY + z * sinY;
            float rz = -x * sinY + z * cosY;
            float ry = y * cosX - rz * sinX;
            float rz2 = y * sinX + rz * cosX;
            float persp = 1f + rz2 * 0.015f;
            return new PointF(cx + rx * scale * persp, cy + (ry - bodyBob) * scale * persp);
        }

        float Depth(float x, float z) => -x * sinY + z * cosY;

        _faces.Clear();
        int faceIdx = 0;

        void AddFace(float shade,
            (float X, float Y, float Z) a, (float X, float Y, float Z) b,
            (float X, float Y, float Z) c, (float X, float Y, float Z) d)
        {
            var pts = new[] { Project(a.X, a.Y, a.Z), Project(b.X, b.Y, b.Z),
                              Project(c.X, c.Y, c.Z), Project(d.X, d.Y, d.Z) };
            float depth = (Depth(a.X, a.Z) + Depth(b.X, b.Z) + Depth(c.X, c.Z) + Depth(d.X, d.Z)) / 4f;
            Color baseColor = _faceColors?[faceIdx] ?? Color.FromArgb(180, 160, 140);
            faceIdx++;
            _faces.Add(new FaceData { Pts = pts, Color = ShiftBrightness(baseColor, shade), Depth = depth });
        }

        void Box(float x0, float x1, float y0, float y1, float z0, float z1)
        {
            AddFace(0f,     (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1));
            AddFace(-0.15f, (x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0));
            AddFace(-0.20f, (x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0));
            AddFace(-0.10f, (x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1));
            AddFace(0.20f,  (x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1));
            AddFace(-0.30f, (x0, y0, z1), (x1, y0, z1), (x1, y0, z0), (x0, y0, z0));
        }

        // Tête 8x8x8
        Box(-4, 4, -12, -4, -4, 4);
        // Corps 8x12x4
        Box(-4, 4, -4, 8, -2, 2);
        // Bras gauche
        {
            float rot = -armSwing;
            float ox = MathF.Sin(rot) * 3f, oz = MathF.Cos(rot) * -1f;
            Box(-6 + ox, -2 + ox, -4, 8, -2 + oz, 2 + oz);
        }
        // Bras droit
        {
            float rot = armSwing;
            float ox = MathF.Sin(rot) * -3f, oz = MathF.Cos(rot) * 1f;
            Box(2 + ox, 6 + ox, -4, 8, -2 + oz, 2 + oz);
        }
        // Jambe gauche
        {
            float ox = MathF.Sin(legSwing) * 2f, oz = MathF.Cos(legSwing) * -1f;
            Box(-4 + ox, ox, 8, 20, -2 + oz, 2 + oz);
        }
        // Jambe droite
        {
            float ox = MathF.Sin(-legSwing) * -2f, oz = MathF.Cos(-legSwing) * 1f;
            Box(ox, 4 + ox, 8, 20, -2 + oz, 2 + oz);
        }

        // Sort by depth (painter's algorithm)
        if (_faces.Count > 1)
        {
            var arr = _faces.ToArray();
            Array.Sort(arr, (a, b) => b.Depth.CompareTo(a.Depth));
            foreach (var f in arr)
            {
                if (f.Pts == null || f.Pts.Length < 3) continue;
                try
                {
                    _outlinePen.Color = ControlPaint.Dark(f.Color, 0.2f);
                    using var brush = GetBrush(f.Color);
                    g.FillPolygon(brush, f.Pts);
                    g.DrawPolygon(_outlinePen, f.Pts);
                }
                catch { }
            }
        }

        TextRenderer.DrawText(g, "Glisser = tourner | Molette = zoom",
            _infoFont, new Point(4, 4), Color.FromArgb(100, 200, 200, 200));
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (e.Button is MouseButtons.Left or MouseButtons.Right)
        {
            _dragging = true;
            _lastMouse = e.Location;
            _timer.Stop();
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (!_dragging) return;
        _rotY += (e.X - _lastMouse.X) * 0.01f;
        _rotX = Math.Clamp(_rotX + (e.Y - _lastMouse.Y) * 0.01f, -1f, 1f);
        _lastMouse = e.Location;
        Invalidate();
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        _dragging = false;
        if (_skin != null && !_walking) StartTimer();
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        _zoom = Math.Clamp(_zoom + e.Delta * 0.001f, 0.3f, 3f);
        Invalidate();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _timer.Stop(); _timer.Dispose();
            _skin?.Dispose();
            _outlinePen.Dispose();
            _infoFont.Dispose();
            foreach (var b in _brushCache.Values) b.Dispose();
            _brushCache.Clear();
        }
        base.Dispose(disposing);
    }

    private SolidBrush GetBrush(Color color)
    {
        if (!_brushCache.TryGetValue(color, out var brush))
        {
            brush = new SolidBrush(color);
            _brushCache[color] = brush;
        }
        return brush;
    }
}
