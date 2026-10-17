#include "ui_internal.hpp"

#include "qrcodegen.hpp"

#include <cmath>
#include <vector>

#include "game_launcher.hpp" // log_line : une trace dans launcher.log
#include "http_win.hpp"      // host_allowed : allowlist S2
#include "ms_auth.hpp"
#include "proc.hpp" // open_detached (POSIX) ; vide sous Windows

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#endif

// ---------------------------------------------------------------------------
// Modale de connexion Microsoft (portage de MsAuth.ShowCodeDialog, 520x360).
// Vit cote UI : ms_auth.cpp (tl_core) ne connait pas ImGui.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

// Lien encodé dans le QR code. Si Microsoft n'a pas fourni de lien avec le
// code pré-rempli (verification_uri_complete), on ajoute le paramètre
// « otc » de microsoft.com/link ; si la page l'ignorait, le joueur
// verrait simplement le champ vide et taperait le code affiché à l'écran.
std::string qr_target(const std::string& url, const std::string& code) {
    if (url.find("otc=") != std::string::npos || url.find(code) != std::string::npos)
        return url;
    return url + (url.find('?') == std::string::npos ? "?otc=" : "&otc=") + code;
}

// Dessine le QR code centré, sur fond blanc avec une marge de 4 modules
// (indispensable à la lecture par un téléphone). Calculé une fois par lien.
void draw_login_qr(const std::string& text, float width) {
    static std::string cachedText;
    static std::vector<bool> modules;
    static int n = 0;
    if (text != cachedText) {
        cachedText = text;
        modules.clear();
        n = 0;
        try {
            const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            n = qr.getSize();
            modules.resize(static_cast<size_t>(n) * n);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) modules[static_cast<size_t>(y) * n + x] = qr.getModule(x, y);
        } catch (const std::exception&) {
            n = 0; // lien trop long : pas de QR, le code reste affiché
        }
    }
    if (n == 0) return;
    const int quiet = 4;
    const float side = 156.0f;
    const float cell = std::floor(side / static_cast<float>(n + 2 * quiet));
    const float real = cell * static_cast<float>(n + 2 * quiet);
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const ImVec2 p(cur.x + (width - real) * 0.5f, cur.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + real, p.y + real), IM_COL32(255, 255, 255, 255), 6.0f);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (modules[static_cast<size_t>(y) * n + x]) {
                const float x0 = p.x + (x + quiet) * cell, y0 = p.y + (y + quiet) * cell;
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + cell, y0 + cell),
                                  IM_COL32(0, 0, 0, 255));
            }
    ImGui::Dummy(ImVec2(width, real + 6.0f));
}

} // namespace


// Hote d'une URL, en minuscules, sans port ni chemin. Vide si l'URL n'est
// pas en https — tout le reste est refuse d'office.
std::string https_host(const std::string& url) {
    if (url.size() < 9) return {};
    std::string head = url.substr(0, 8);
    for (auto& ch : head) ch = static_cast<char>(std::tolower((unsigned char)ch));
    if (head != "https://") return {};
    std::string host = url.substr(8);
    for (const char sep : {'/', '?', '#'}) {
        const auto p = host.find(sep);
        if (p != std::string::npos) host.resize(p);
    }
    if (const auto p = host.find('@'); p != std::string::npos)
        host.erase(0, p + 1); // « user:pass@vrai-hote » : on garde l'hote reel
    if (const auto p = host.find(':'); p != std::string::npos) host.resize(p);
    for (auto& ch : host) ch = static_cast<char>(std::tolower((unsigned char)ch));
    return host;
}

// Ouvre un lien dans le navigateur de l'utilisateur.
//
// Defaut vecu : cette fonction reutilisait browser_url_allowed(), le garde
// du flux d'authentification Microsoft, qui n'autorise que login.live.com
// et consorts. TOUS les autres liens etaient donc refuses — la page Aide
// en entier, le visualiseur de modeles, et les deux boutons « Ouvrir la
// page de la version ». En silence, sans trace ni message : pour
// l'utilisateur, des boutons qui ne font rien.
//
// Le garde reste, avec la bonne politique :
//  - https exclusivement. C'est la vraie surface d'attaque d'un
//    ShellExecute sur une chaine venue d'ailleurs : file://, javascript:,
//    et les gestionnaires de protocole du systeme.
//  - hote dans l'allowlist S2, la meme que pour les requetes sortantes.
//    Plusieurs de ces URL viennent de donnees distantes (page d'une
//    release, flux d'actus) : elles ne sont pas de confiance.
//  - un refus se VOIT. Un bouton muet est un bug ; un bouton qui dit
//    pourquoi il refuse est un comportement.
void open_url(const std::string& url) {
    const std::string host = https_host(url);
    if (host.empty() || !http::host_allowed(host)) {
        log_line("Lien refusé (hôte non autorisé ou URL non https) : " +
                 http::redact_url(url));
        notify_toast(tr("Lien non ouvert", "Link not opened"),
                     host.empty()
                         ? tr("Seuls les liens https sont ouverts.",
                              "Only https links are opened.")
                         : tr("Hôte non autorisé : ", "Host not allowed: ") +
                               host);
        return;
    }
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (n <= 1) return;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    proc::open_detached(url);
#endif
}

// Reaction a une connexion reussie : pseudo + mode compte + toast, une fois.
// Fidele au C# (AccountPage et PlayCore ecrivent tous deux PlayerName).
void auth_sync() {
    if (auth::get_state() != auth::AuthState::Done) return;
    if (auto s = auth::get_session()) {
        auto& st = DataStore::settings;
        if (st.accountMode != "microsoft" || st.playerName != s->name) {
            st.accountMode = "microsoft";
            st.playerName = s->name;
            DataStore::save();
        }
        notify_toast(tr("Connecté à Microsoft"),
                     tr("Bienvenue ", "Welcome ") + s->name +
                         tr(" ! Ton pseudo et ton skin officiels seront utilisés en jeu.", "! Your official username and skin will be used in-game."));
    }
    auth::dismiss();
}

} // namespace tl::ui

namespace tl::auth {

void draw_login_modal() {
    using namespace tl::ui;

    static bool wasOpen = false;
    static bool copied = false; // « Copier le code » : remis a zero a l'ouverture
    static bool pendingOpen = false;
    const bool want = get_state() != AuthState::Idle;
    if (want && !wasOpen) pendingOpen = true;
    wasOpen = want;
    if (!want) {
        pendingOpen = false;
        return;
    }
    if (pendingOpen) {
        // OpenPopup remplacerait toute popup deja ouverte (elles partagent le
        // meme niveau 0) : on attend qu'il n'y en ait plus pour ouvrir la notre.
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) return;
        pendingOpen = false;
        ImGui::OpenPopup("###msauth");
        copied = false;
    }

    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    // Plus haute pendant l'attente du code : elle porte alors le QR code.
    const bool showsQr = get_state() == AuthState::WaitingCode;
    ImGui::SetNextWindowSize(ImVec2(520, showsQr ? 560.0f : 360.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kCard);

    const std::string mTitle = std::string(tr("Connexion Microsoft")) + "###msauth";
    if (ImGui::BeginPopupModal(mTitle.c_str(), nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar)) {
        const AuthState st = get_state();
        if (st == AuthState::Idle) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleColor();
            return;
        }
        const LoginInfo info = get_login_info().value_or(LoginInfo{});
        const float w = ImGui::GetContentRegionAvail().x;

        auto centered = [&](const char* text, const ImVec4& col) {
            const float tw = ImGui::CalcTextSize(text).x;
            // Jamais de decalage negatif : ProggyClean est large, un texte plus
            // long que la modale sortirait des deux cotes.
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (tw < w ? (w - tw) * 0.5f : 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
        };

        if (st == AuthState::Error) {
            centered(tr("Échec de la connexion Microsoft"), kDanger);
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushTextWrapPos(w);
            ImGui::TextUnformatted(info.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 56.0f);
            if (accent_button(tr("Réessayer"), ImVec2(170, 38))) {
                dismiss();
                login_start(/*force=*/false);
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Fermer"), ImVec2(170, 38))) dismiss();
            ImGui::EndPopup();
            ImGui::PopStyleColor();
            return;
        }

        if (st == AuthState::WaitingCode && !info.userCode.empty()) {
            // QR code à scanner avec le téléphone : il ouvre la page de
            // connexion Microsoft avec le code déjà rempli.
            draw_login_qr(qr_target(info.verificationUrl, info.userCode), w);
            centered(tr("Scanne ce QR code avec ton téléphone, ou bien :",
                        "Scan this QR code with your phone, or:"), kDim);
            ImGui::Spacing();
            // Deux lignes courtes : le texte C# d'origine tenait en 520 px en
            // Segoe UI, pas dans la police ImGui par defaut (bien plus large).
            centered(tr("Ton navigateur s'est ouvert sur la page"), kText);
            centered(tr("de connexion Microsoft. Entre ce code :"), kText);
            ImGui::Spacing();

            // Le code en tres gros, impossible a rater (C# Consolas 34 gras).
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float boxH = 84.0f;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + boxH),
                              ImGui::ColorConvertFloat4ToU32(kBg), 8.0f);
            dl->AddRect(p, ImVec2(p.x + w, p.y + boxH),
                        ImGui::ColorConvertFloat4ToU32(kBorder), 8.0f, 0, 1.0f);
            ImFont* f = fBig ? fBig : ImGui::GetFont();
            const float codeSize = 40.0f;
            const ImVec2 cs = f->CalcTextSizeA(codeSize, FLT_MAX, 0.0f,
                                               info.userCode.c_str());
            dl->AddText(f, codeSize,
                        ImVec2(p.x + (w - cs.x) * 0.5f, p.y + (boxH - cs.y) * 0.5f),
                        ImGui::ColorConvertFloat4ToU32(kAccent), info.userCode.c_str());
            ImGui::Dummy(ImVec2(w, boxH + 10.0f));

            if (accent_button(tr("Rouvrir la page"), ImVec2(170, 38)))
                open_url(info.verificationUrl);
            ImGui::SameLine();
            if (ImGui::Button(copied ? tr("Code copié") : tr("Copier le code"),
                              ImVec2(170, 38))) {
                ImGui::SetClipboardText(info.userCode.c_str());
                copied = true;
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushTextWrapPos(w);
            ImGui::TextUnformatted(
                (tr("Si la page ne s'est pas ouverte : ", "If the page did not open: ") + info.verificationUrl).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        } else {
            // RequestingCode / Chaining : pas encore de code a afficher.
            ImGui::Dummy(ImVec2(w, 90.0f));
            // Les etats de ms_auth.cpp sont en francais : passage au dictionnaire.
            const std::string step =
                info.message.empty() ? tr("Connexion en cours...") : tr(info.message);
            centered(step.c_str(), kText);
            ImGui::Spacing();
            centered(tr("Cela peut prendre quelques secondes."), kDim);
        }

        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 56.0f);
        if (ImGui::Button(tr("Annuler"), ImVec2(170, 38))) {
            cancel_login();
            dismiss();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

} // namespace tl::auth
