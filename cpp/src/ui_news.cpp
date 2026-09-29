#include "ui_internal.hpp"

#include "http_win.hpp"
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
// Page Actualités (fidele NewsPage C#) : « Voir le site web », flux distant
// (NewsUrl, cache offline news-cache.json) + historique local changelog.json.
// Fidélité : comme C#, le panneau « Dernières actualités » contient le
// changelog local d'abord, puis les actus distantes (anomalie C# conservée).
// ---------------------------------------------------------------------------

namespace {

std::vector<NewsEntry> parse_news_array(const std::string& text) {
    std::vector<NewsEntry> out;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_array()) return out;
        for (const auto& e : j) {
            if (!e.is_object()) continue;
            NewsEntry n;
            n.title = e.value("title", e.value("Title", ""));
            n.date = e.value("date", e.value("Date", ""));
            n.tag = e.value("tag", e.value("Tag", ""));
            n.text = e.value("text", e.value("Text", ""));
            out.push_back(std::move(n));
        }
    } catch (...) {}
    return out;
}

// Seed C# (Changelog.DefaultEntries). Glyphes hors Latin-1 (• ✎ 🔗) remplaces
// par du texte car la police ImGui par defaut ne les couvre pas.
std::vector<NewsEntry> default_changelog() {
    return {
        {"Sidebar compacte et minimaliste", "2026-08-27", "NOUVEAU",
         "- Sidebar reduite a 56px avec icones emoji\n"
         "- Profil au-dessus de Compte / Parametres\n"
         "- Tooltips au survol"},
        {"Page Instances en cartes", "2026-08-27", "NOUVEAU",
         "- Grille de cartes horizontales\n"
         "- Clic -> page detail, Editer pour modifier\n"
         "- Bouton Jouer sur chaque carte"},
        {"Page détail instance style CurseForge", "2026-08-27", "NOUVEAU",
         "- Banniere info complete\n"
         "- Onglets Description / Mods / Mondes / Shaders / RP / Screenshots"},
        {"Partage avec code court", "2026-08-27", "NOUVEAU",
         "- Bouton Partager -> zip ou code court\n"
         "- Code type CurseForge (ex: ABCD-EFGH)"},
        {"Import CurseForge par URL", "2026-08-27", "NOUVEAU",
         "- URL, ID ou slug -> installation auto du modpack"},
        {"Explorateur et Skins refaits", "2026-08-27", "FIX",
         "- Explorateur : style moderne en liste\n"
         "- Skins : thumbnails arrondies, propre"},
    };
}

// Changelog.GetAll : fichier (PascalCase) + seed si absent/vide.
// Les entrees vides {} (residu observe) sont ignorees.
std::vector<NewsEntry> load_changelog() {
    std::vector<NewsEntry> out;
    const std::filesystem::path p = DataStore::dir() / "changelog.json";
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        std::ifstream in(p);
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        try {
            const nlohmann::json j = nlohmann::json::parse(text);
            if (j.is_array())
                for (const auto& e : j) {
                    if (!e.is_object()) continue;
                    NewsEntry n;
                    n.title = e.value("Title", e.value("title", ""));
                    n.date = e.value("Date", e.value("date", ""));
                    n.tag = e.value("Tag", e.value("tag", ""));
                    n.text = e.value("Text", e.value("text", ""));
                    if (n.title.empty() && n.text.empty()) continue;
                    out.push_back(std::move(n));
                }
        } catch (...) {}
    }
    if (out.empty()) {
        out = default_changelog();
        try {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& e : out)
                arr.push_back({{"Title", e.title},
                               {"Date", e.date},
                               {"Tag", e.tag},
                               {"Text", e.text}});
            std::filesystem::create_directories(p.parent_path(), ec);
            std::ofstream ofs(p, std::ios::trunc);
            ofs << arr.dump(4);
        } catch (...) {}
    }
    return out;
}

std::string trimmed_copy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// Thread de chargement (NewsService.GetAsync), suivi dans le panneau AppTasks.
void news_worker(int tid) {
    std::vector<NewsEntry> changelog = load_changelog();
    std::vector<NewsEntry> remote;
    const std::string url = trimmed_copy(DataStore::settings.newsUrl);
    if (!url.empty()) {
        const auto resp = http::get_string(url, &newsState.cancel);
        if (resp) {
            remote = parse_news_array(*resp);
            if (!remote.empty()) {
                try {
                    std::ofstream ofs(DataStore::dir() / "news-cache.json",
                                      std::ios::trunc);
                    ofs << *resp;
                } catch (...) {}
            }
        } else {
            // echec -> cache offline
            std::ifstream in(DataStore::dir() / "news-cache.json");
            if (in) {
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                remote = parse_news_array(text);
            }
        }
    }
    std::vector<NewsEntry> items = changelog;
    items.insert(items.end(), remote.begin(), remote.end());
    {
        std::lock_guard<std::mutex> lk(newsState.m);
        newsState.changelog = std::move(changelog);
        newsState.news = std::move(items);
        newsState.loading = false;
        newsState.loaded = true;
    }
    // Annulation panneau : l'entree reste « Annulee », sinon Done.
    if (newsState.cancel.load()) (void)tl::tasks::cancel(tid);
    apptasks_end(tid);
}

// Carte (actu ou changelog) : meme carte C#, hauteur calculee (wrap 700)
void news_card(const NewsEntry& e, bool asChangelog, int idx) {
    const float availW = ImGui::GetContentRegionAvail().x;
    const float wrapW = std::min(700.0f, std::max(120.0f, availW - 40.0f));
    const float textH =
        ImGui::CalcTextSize(e.text.c_str(), nullptr, false, wrapW).y;
    const float h = 16.0f + 16.0f + 6.0f + 20.0f + 6.0f + textH + 16.0f;
    ImGui::PushID(idx);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##card", ImVec2(0, h), ImGuiChildFlags_Borders);
    float x0 = ImGui::GetCursorScreenPos().x + 20.0f;
    float y = ImGui::GetCursorScreenPos().y + 16.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ligne du haut : date (+ badge tag)
    if (asChangelog) {
        // C# : "Date" ou "Date  •  Tag" (12 dim) — pas de badge
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (fSmall) ImGui::PushFont(fSmall);
        if (e.tag.empty())
            ImGui::TextUnformatted(e.date.c_str());
        else
            ImGui::Text("%s  ·  %s", e.date.c_str(), e.tag.c_str());
        if (fSmall) ImGui::PopFont();
        ImGui::PopStyleColor();
        y += 16.0f + 6.0f;
        // titre (C# 15 SemiBold accent)
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(e.title.c_str());
        ImGui::PopStyleColor();
        y += 20.0f + 6.0f;
    } else {
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (fSmall) ImGui::PushFont(fSmall);
        ImGui::TextUnformatted(e.date.c_str());
        if (fSmall) ImGui::PopFont();
        ImGui::PopStyleColor();
        if (!e.tag.empty()) {
            // badge accent (fond accent, texte blanc, radius 6)
            const ImVec2 ts = fSmall ? fSmall->CalcTextSizeA(
                                           fSmall->FontSize, 1000.0f, 0.0f,
                                           e.tag.c_str())
                                     : ImGui::CalcTextSize(e.tag.c_str());
            const ImVec2 b0(x0 + 60.0f, y - 3.0f);
            const ImVec2 b1(b0.x + ts.x + 16.0f, b0.y + 17.0f);
            dl->AddRectFilled(b0, b1,
                              ImGui::ColorConvertFloat4ToU32(kAccent), 6.0f);
            dl->AddText(ImVec2(b0.x + 8.0f, b0.y + 2.0f),
                        IM_COL32(255, 255, 255, 255), e.tag.c_str());
        }
        y += 16.0f + 6.0f;
        // titre (C# 16 SemiBold)
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::TextUnformatted(e.title.c_str());
        y += 20.0f + 6.0f;
    }

    // texte (13 dim, wrap)
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::BeginChild("##ctext", ImVec2(wrapW, textH + 4.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar |
                                                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos());
    ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + wrapW);
    ImGui::TextUnformatted(e.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

} // namespace

void news_page() {
    // Declenchement (C# : page instanciee au demarrage -> ici 1re visite),
    // suivi comme tache de fond annulable.
    {
        std::lock_guard<std::mutex> lk(newsState.m);
        if (!newsState.loading && !newsState.loaded) {
            newsState.loading = true;
            newsState.cancel.store(false);
            const int tid = newsState.taskId =
                apptasks_begin("Actualités", "", &newsState.cancel);
            newsState.th = std::thread(news_worker, tid);
        }
    }

    ImGui::BeginChild("##newsscroll", ImVec2(0, 0));

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Actualités"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    if (accent_button(tr("Voir le site web"), ImVec2(160, 34))) {
        std::string url = trimmed_copy(DataStore::settings.newsUrl);
        if (url.rfind("http", 0) != 0)
            url = "https://teamstarwars-dev.github.io/Team-Luncher-/";
#ifdef _WIN32
        const std::wstring w(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
#else
        proc::open_detached(url);
#endif
    }
    ImGui::Spacing();

    ImGui::TextUnformatted(tr("Dernières actualités"));
    ImGui::Spacing();

    bool loading = false;
    std::vector<NewsEntry> news, changelog;
    {
        std::lock_guard<std::mutex> lk(newsState.m);
        loading = newsState.loading && !newsState.loaded;
        news = newsState.news;
        changelog = newsState.changelog;
    }
    if (loading) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Chargement..."));
        ImGui::PopStyleColor();
    } else if (news.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Aucune actualité pour l'instant."));
        ImGui::PopStyleColor();
    } else {
        int idx = 0;
        for (const auto& e : news) news_card(e, false, idx++);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Historique des versions"));
    ImGui::Spacing();
    if (changelog.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Chargement..."));
        ImGui::PopStyleColor();
    } else {
        int idx = 1000;
        for (const auto& e : changelog) news_card(e, true, idx++);
    }

    ImGui::EndChild();
}

} // namespace tl::ui
