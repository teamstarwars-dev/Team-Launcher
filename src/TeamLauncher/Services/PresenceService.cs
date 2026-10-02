using DiscordRPC;

namespace TeamLauncher;

/// <summary>
/// Rich Presence Discord personnalisée de Team Launcher :
/// logo, nom du launcher, temps de jeu en direct + temps total sur l'instance.
///
/// Discord peut couper le lien RPC à tout moment (redémarrage, mise à jour,
/// fermeture…) : un chien de garde réinitialise la connexion automatiquement.
///
/// Pour le logo : discord.com/developers/applications → Rich Presence →
/// Art Assets → téléverser le logo sous le nom EXACT « logo » → Save Changes,
/// puis coller l'ID d'application dans Paramètres.
/// </summary>
public static class PresenceService
{
    private static DiscordRpcClient? _client;
    private static DateTime? _sessionStart;
    private static bool _ready;
    private static long _lastAttemptTicks;
    private static System.Threading.Timer? _watchdog;

    public static bool Enabled =>
        DataStore.Settings.DiscordEnabled &&
        !string.IsNullOrWhiteSpace(DataStore.Settings.DiscordAppId);

    public static void Init()
    {
        if (!Enabled) return;
        StartWatchdog();
        TryConnect();
    }

    private static void TryConnect()
    {
        if (!Enabled || _ready && _client != null) return;
        _lastAttemptTicks = Environment.TickCount64;
        try
        {
            ShutdownInternal();

            _client = new DiscordRpcClient(DataStore.Settings.DiscordAppId.Trim());
            _client.OnReady += (_, _) =>
            {
                _ready = true;
                Log("Discord : connecté.");
                SetLauncherPresence();
            };
            _client.OnClose += (_, _) =>
            {
                _ready = false;
                Log("Discord : connexion fermée (le chien de garde va relancer).");
            };
            _client.OnConnectionFailed += (_, _) =>
            {
                _ready = false;
                Log("Discord : connexion échouée (Discord est-il ouvert ?).");
            };

            _client.Initialize();
            if (!_client.IsInitialized)
            {
                Log("Discord : initialisation impossible.");
                _client = null;
            }
        }
        catch (Exception ex)
        {
            Log("Discord : erreur d'initialisation : " + ex.Message);
            _client = null;
        }
    }

    /// <summary>Toutes les 20 s : si le lien est mort, on reconnecte.</summary>
    private static void StartWatchdog()
    {
        if (_watchdog != null) return;
        _watchdog = new System.Threading.Timer(_ =>
        {
            try
            {
                if (!Enabled) return;
                bool connected = _ready && _client != null;
                if (!connected && Environment.TickCount64 - _lastAttemptTicks > 20000)
                    TryConnect();
            }
            catch { }
        }, null, 5000, 20000);
    }

    public static void Shutdown()
    {
        try { _client?.ClearPresence(); } catch { }
        ShutdownInternal();
    }

    private static void ShutdownInternal()
    {
        try { _client?.Dispose(); } catch { }
        _client = null;
        _ready = false;
        _sessionStart = null;
    }

    /// <summary>Présence pendant qu'une instance tourne (chrono + temps total + ville RP si multi).</summary>
    public static void UpdateGame(InstanceInfo inst)
    {
        if (!Enabled || _client == null || !_ready) return;
        _sessionStart ??= DateTime.UtcNow;

        // serveur rejoint ? on cherche la ville RP correspondante
        string? server = GameLauncher.CurrentServer;
        string? cityName = null, state = "Team Launcher";
        if (server != null)
        {
            string host = server.Split(':')[0];
            var city = DataStore.Settings.Cities.FirstOrDefault(c =>
            {
                string addr = c.Address.Split(':')[0];
                return addr.Equals(host, StringComparison.OrdinalIgnoreCase);
            });
            cityName = city?.Name;
            state = cityName != null
                ? $"🏘 {cityName} — ville de {city!.Owner}"
                : $"En multijoueur : {server}";
        }
        long totalHours = (long)Math.Floor(inst.PlaySeconds / 3600.0);

        try
        {
            _client.SetPresence(new RichPresence
            {
                Details = $"Joue à {inst.Name}",
                State = state,
                Timestamps = new Timestamps(_sessionStart.Value),
                Assets = new Assets
                {
                    LargeImageKey = "logo",
                    LargeImageText = "Team Launcher",
                    SmallImageKey = "logo",
                    SmallImageText = $"{inst.Loader} • Minecraft {inst.McVersion}" +
                                     (totalHours > 0 ? $" • {totalHours} h" : "")
                },
                Buttons = new[]
                {
                    new DiscordRPC.Button { Label = "🌐 Visiter le site", Url = "https://team-launcher.vercel.app" }
                }
            });
        }
        catch { }
    }

    /// <summary>Présence quand aucune partie ne tourne.</summary>
    public static void SetLauncherPresence()
    {
        if (!Enabled || _client == null || !_ready) return;
        long totalSeconds = DataStore.Settings.Instances.Sum(i => i.PlaySeconds);
        var ts = TimeSpan.FromSeconds(totalSeconds);
        string hours = ts.TotalHours >= 1
            ? $"{(int)ts.TotalHours} h {ts.Minutes} min" : $"{ts.Minutes} min";

        try
        {
            _client.SetPresence(new RichPresence
            {
                Details = "Dans le launcher",
                State = $"Temps de jeu total : {hours}",
                Assets = new Assets
                {
                    LargeImageKey = "logo",
                    LargeImageText = "Team Launcher"
                },
                Buttons = new[]
                {
                    new DiscordRPC.Button { Label = "🌐 Visiter le site", Url = "https://team-launcher.vercel.app" }
                }
            });
        }
        catch { }
    }

    private static void Log(string text)
    {
        try
        {
            File.AppendAllText(Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "TeamLauncher", "launcher.log"), $"[{DateTime.Now:HH:mm:ss}] [Presence] {text}\n");
        }
        catch { }
    }
}
