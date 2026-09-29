#include "ui_internal.hpp"

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

void open_url(const std::string& url) {
    // Même garde que open_browser côté flux : l'URL vient du JSON serveur.
    if (!auth::detail::browser_url_allowed(url)) return;
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
    ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_Always);
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
