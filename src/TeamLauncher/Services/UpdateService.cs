using Velopack;

namespace TeamLauncher;

public static class UpdateService
{
    private static UpdateManager? _updateManager;

    public static string CurrentVersion =>
        System.Reflection.Assembly.GetExecutingAssembly().GetName().Version?.ToString(4) ?? "1.0.0.0";

    private static UpdateManager GetUpdateManager()
    {
        return _updateManager ??= new UpdateManager(
            "https://github.com/teamstarwars-dev/Team-Launcher/releases"
        );
    }

    public static async Task CheckOnStartupAsync()
    {
        try
        {
            var um = GetUpdateManager();
            var info = await um.CheckForUpdatesAsync();
            if (info == null) return;

            var mainForm = Application.OpenForms.OfType<Form>().FirstOrDefault();
            if (mainForm == null) return;

            string notes = info.TargetFullRelease.NotesMarkdown ?? "";

            var result = mainForm.Invoke(() => MessageBox.Show(mainForm,
                $"Une mise à jour est disponible : v{info.TargetFullRelease.Version}\n\n" +
                $"Tu es en v{CurrentVersion}\n\n" +
                $"Changelog :\n{notes}\n\n" +
                $"Mettre à jour maintenant ?",
                "Team Launcher — Mise à jour",
                MessageBoxButtons.YesNo, MessageBoxIcon.Information));

            if (result == DialogResult.Yes)
            {
                try
                {
                    SetProgress(mainForm, "Téléchargement…");
                    await um.DownloadUpdatesAsync(info);

                    SetProgress(mainForm, "Installation…");
                    um.ApplyUpdatesAndRestart(info.TargetFullRelease);
                }
                catch (Exception ex)
                {
                    mainForm.BeginInvoke(() =>
                    {
                        MessageBox.Show(mainForm, "Erreur lors de la mise à jour :\n" + ex.Message,
                            "Team Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        mainForm.Text = "Team Launcher";
                    });
                }
            }
        }
        catch { }
    }

    public static async Task<(string Version, string Notes)?> CheckAsync()
    {
        try
        {
            var um = GetUpdateManager();
            var info = await um.CheckForUpdatesAsync();
            if (info == null) return null;

            return (info.TargetFullRelease.Version.ToString(), info.TargetFullRelease.NotesMarkdown ?? "");
        }
        catch { return null; }
    }

    public static async Task UpdateAsync(Form owner)
    {
        var um = GetUpdateManager();
        var info = await um.CheckForUpdatesAsync();
        if (info == null)
            throw new Exception("Aucune mise à jour disponible.");

        SetProgress(owner, "Téléchargement…");
        await um.DownloadUpdatesAsync(info);

        SetProgress(owner, "Installation et relance…");
        um.ApplyUpdatesAndRestart(info.TargetFullRelease);
    }

    public static async Task PromptUpdateAsync(Form owner)
    {
        var update = await CheckAsync();
        if (update == null)
        {
            MessageBox.Show(owner, "Tu es à jour !", "Team Launcher");
            return;
        }

        var result = MessageBox.Show(owner,
            $"Nouvelle version disponible : v{update.Value.Version}\n\n" +
            $"Tu es en v{CurrentVersion}\n\n" +
            $"Changelog :\n{update.Value.Notes}\n\n" +
            $"Mettre à jour maintenant ?",
            "Team Launcher — Mise à jour",
            MessageBoxButtons.YesNo, MessageBoxIcon.Information);

        if (result == DialogResult.Yes)
        {
            try
            {
                await UpdateAsync(owner);
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, "Erreur :\n" + ex.Message, "Team Launcher");
                owner.Text = "Team Launcher";
            }
        }
    }

    private static void SetProgress(Form form, string msg)
    {
        try { form.BeginInvoke(() => form.Text = $"Team Launcher — {msg}"); } catch { }
    }
}
