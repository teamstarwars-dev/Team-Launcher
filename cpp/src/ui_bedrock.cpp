#include "ui_internal.hpp"

#include "proc.hpp" // open_detached (POSIX) ; vide sous Windows

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#endif

#include <fstream>

namespace tl::ui {

// ---------------------------------------------------------------------------
// Page Bedrock (fidele BedrockPage C#) : statut UWP, lancement minecraft:,
// optimisation options.txt (backup + restauration), Microsoft Store.
// 100 % local, aucun appel réseau.
// ---------------------------------------------------------------------------

namespace {

#ifdef _WIN32
std::filesystem::path local_appdata() {
    wchar_t buf[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return n ? std::filesystem::path(buf) : std::filesystem::path();
}
#endif

// %LOCALAPPDATA%\Packages\Microsoft.MinecraftUWP_8wekyb3d8bbwe (Windows).
// Sous Linux : chemin vide (bedrock_installed() est toujours faux).
std::filesystem::path bedrock_root() {
#ifdef _WIN32
    return local_appdata() / L"Packages" /
           L"Microsoft.MinecraftUWP_8wekyb3d8bbwe";
#else
    return {};
#endif
}

std::filesystem::path options_path() {
    return bedrock_root() / L"LocalState" / L"games" / L"com.mojang" /
           L"minecraftpe" / L"options.txt";
}

std::filesystem::path options_backup() {
    return std::filesystem::path(options_path().wstring() + L".backup-original");
}

bool bedrock_installed() {
    std::error_code ec;
    return std::filesystem::is_directory(bedrock_root(), ec);
}

#ifdef _WIN32
void shell_open(const wchar_t* uri) {
    ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}
#else
void shell_open(const char* uri) { proc::open_detached(uri ? uri : ""); }
#endif

// Ecrit/remplace les cles key:value (C# optimiser : add si absente, sinon replace)
void write_option_keys(const std::vector<std::pair<const char*, const char*>>& kvs) {
    const std::filesystem::path p = options_path();
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::vector<std::string> lines;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        const std::string key =
            colon == std::string::npos ? line : line.substr(0, colon);
        bool replaced = false;
        for (const auto& kv : kvs)
            if (key == kv.first) {
                lines.push_back(std::string(kv.first) + ":" + kv.second);
                replaced = true;
                break;
            }
        if (!replaced) lines.push_back(line);
    }
    in.close();
    for (const auto& kv : kvs) {
        bool found = false;
        for (const auto& l : lines)
            if (l.rfind(std::string(kv.first) + ":", 0) == 0) {
                found = true;
                break;
            }
        if (!found) lines.push_back(std::string(kv.first) + ":" + kv.second);
    }
    std::ofstream out(p, std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
}

} // namespace

void bedrock_page() {
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("CHANGER DE MINECRAFT"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Ton launcher gère Minecraft JAVA. Bascule ici sur Minecraft BEDROCK\n"
           "(édition Microsoft Store, cross-play mobile / console / PC)."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const bool installed = bedrock_installed();

    // ---- Bloc statut ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##bstatus", ImVec2(0, 56), ImGuiChildFlags_Borders);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + 18.0f,
                                     ImGui::GetCursorScreenPos().y + 14.0f));
    if (installed) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(tr("Minecraft Bedrock est installé sur ce PC"));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(tr("Minecraft Bedrock n'est pas installé"));
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // ---- Boutons pleine largeur (C# padding 16,14 radius 10) ----
    auto fullBtn = [](const char* label, bool accent) {
        ImGui::Spacing();
        if (accent)
            return accent_button(label, ImVec2(-1.0f, 44.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, kCard);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kButtonActive);
        const bool r = ImGui::Button(label, ImVec2(-1.0f, 44.0f));
        ImGui::PopStyleColor(3);
        return r;
    };

    ImGui::BeginDisabled(!installed);
    if (fullBtn(tr("LANCER MINECRAFT BEDROCK"), true)) {
        if (bedrock_installed())
#ifdef _WIN32
            shell_open(L"minecraft:");
#else
            shell_open("minecraft:");
#endif
        else
            notify_toast(tr("Bedrock absent"),
                         tr("Minecraft Bedrock n'est pas installé sur ce PC.\n"
                            "Installe-le d'abord via le bouton Microsoft Store."));
    }
    ImGui::EndDisabled();
    if (fullBtn(tr("OPTIMISER LES PERFORMANCES"), false)) {
#ifndef _WIN32
        // Sans Bedrock UWP, options_path() est vide : ne rien écrire.
        if (!installed) {
            notify_toast(tr("Bedrock absent"),
                         tr("Optimisation indisponible sous Linux."));
        } else
#endif
        try {
            std::error_code ec;
            const std::filesystem::path bak = options_backup();
            if (!std::filesystem::exists(bak, ec)) {
                std::filesystem::create_directories(bak.parent_path(), ec);
                if (std::filesystem::exists(options_path(), ec))
                    std::filesystem::copy_file(options_path(), bak, ec);
            }
            write_option_keys({{"gfx_viewdistance", "6"},
                               {"gfx_fancygraphics", "0"},
                               {"gfx_particleviewdistance", "4"},
                               {"gfx_viewbobbing", "0"},
                               {"gfx_smoothbrightness", "1"}});
            notify_toast(
                tr("Optimisations appliquées"),
                tr("Distance d'affichage : 6 chunks · Graphismes : rapides\n"
                   "Particules réduites, balancement désactivé.\n"
                   "Réglages d'origine sauvegardés (bouton Restaurer)."));
        } catch (const std::exception& ex) {
            notify_toast(tr("Erreur"), ex.what());
        }
    }
    if (fullBtn(tr("RESTAURER LES RÉGLAGES D'ORIGINE"), false)) {
        std::error_code ec;
        if (!std::filesystem::exists(options_backup(), ec)) {
            notify_toast(tr("Info"), tr("Aucune sauvegarde d'origine trouvée."));
        } else {
            std::filesystem::create_directories(options_path().parent_path(), ec);
            std::filesystem::copy_file(options_backup(), options_path(),
                                       std::filesystem::copy_options::overwrite_existing,
                                       ec);
            notify_toast(tr("Réglages restaurés"),
                         tr("Réglages d'origine restaurés."));
        }
    }
    if (fullBtn(tr("INSTALLER DEPUIS LE MICROSOFT STORE"), false)) {
#ifdef _WIN32
        shell_open(L"ms-windows-store://pdp/?ProductId=9NBLGGH2JHXJ");
#else
        shell_open("ms-windows-store://pdp/?ProductId=9NBLGGH2JHXJ");
#endif
    }

    // ---- Carte info ----
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##binfo", ImVec2(0, 118), ImGuiChildFlags_Borders);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + 18.0f,
                                     ImGui::GetCursorScreenPos().y + 12.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("DIFFÉRENCES JAVA vs BEDROCK :\n"
           "- Java : mods, Forge/Fabric/NeoForge, serveurs communautaires (ton "
           "usage actuel)\n"
           "- Bedrock : cross-play avec mobile/console/PC, Marketplace "
           "officielle, plus fluide sur petites machines\n"
           "- Les mondes, skins et mods ne sont PAS partageables entre les deux "
           "editions"));
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace tl::ui
