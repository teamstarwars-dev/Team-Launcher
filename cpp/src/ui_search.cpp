#include "ui_internal.hpp"

#include "icons.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Recherche globale (Ctrl+K).
//
// Une palette de commandes : on tape quelques lettres, on obtient les
// instances, les pages et les reglages qui correspondent, on valide au
// clavier. C'est le moyen le plus court d'atteindre quelque chose quand on
// ne sait plus dans quel onglet il se trouve — le launcher a quinze pages.
//
// Le filtrage est en SOUS-SEQUENCE et non en sous-chaine : « edcar » trouve
// « Édition de carte ». C'est ce que font les palettes de commandes des
// editeurs, et ça evite d'avoir a se rappeler le libelle exact.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct Hit {
    std::string label;
    std::string hint;   // page ou categorie, affiche en gris
    icons::Id icon = icons::Id::Search;
    int page = -1;      // page a ouvrir
    std::string instId; // si non vide : selectionne cette instance
    int score = 0;
};

struct SearchState {
    bool open = false;
    bool focus = false;   // demande de focus au prochain affichage
    char query[96] = "";
    int sel = 0;          // ligne surlignee
};
SearchState S;

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Retire les accents des lettres latines courantes : taper « edition » doit
// trouver « Édition ». Sans cela, la recherche oblige a composer les
// accents, ce qui va a l'encontre du but.
std::string fold(const std::string& in) {
    static const struct { const char* utf8; char plain; } kMap[] = {
        {"à", 'a'}, {"â", 'a'}, {"ä", 'a'}, {"á", 'a'}, {"ã", 'a'},
        {"ç", 'c'}, {"é", 'e'}, {"è", 'e'}, {"ê", 'e'}, {"ë", 'e'},
        {"î", 'i'}, {"ï", 'i'}, {"í", 'i'}, {"ô", 'o'}, {"ö", 'o'},
        {"ó", 'o'}, {"õ", 'o'}, {"ù", 'u'}, {"û", 'u'}, {"ü", 'u'},
        {"ú", 'u'}, {"ÿ", 'y'}, {"ñ", 'n'},
    };
    const std::string s = lower(in);
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if ((static_cast<unsigned char>(s[i]) & 0xE0) == 0xC0 && i + 1 < s.size()) {
            const std::string two = s.substr(i, 2);
            bool done = false;
            for (const auto& m : kMap)
                if (two == m.utf8) {
                    out.push_back(m.plain);
                    done = true;
                    break;
                }
            if (!done) out += two;
            i += 2;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// Correspondance en sous-sequence. Renvoie un score (plus grand = mieux),
// ou -1 si les lettres n'y sont pas dans l'ordre. Les lettres consecutives
// et les debuts de mot comptent double : « carte » doit classer « Édition
// de carte » avant une correspondance eparpillee.
int subseq_score(const std::string& hayRaw, const std::string& needleRaw) {
    const std::string hay = fold(hayRaw);
    const std::string needle = fold(needleRaw);
    if (needle.empty()) return 0;
    int score = 0, run = 0;
    std::size_t h = 0;
    for (char c : needle) {
        bool found = false;
        while (h < hay.size()) {
            const bool wordStart =
                h == 0 || hay[h - 1] == ' ' || hay[h - 1] == '-' ||
                hay[h - 1] == '(' || hay[h - 1] == '.';
            if (hay[h] == c) {
                score += 1 + run + (wordStart ? 3 : 0);
                ++run;
                ++h;
                found = true;
                break;
            }
            run = 0;
            ++h;
        }
        if (!found) return -1;
    }
    // A qualite egale, le libelle le plus court est le plus pertinent.
    score += static_cast<int>(40 > hay.size() ? 40 - hay.size() : 0) / 4;
    return score;
}

struct Entry {
    const char* fr;
    const char* en;
    int page;
    icons::Id icon;
};

// Pages et reglages atteignables. L'ordre n'importe pas, le score classe.
const Entry kPages[] = {
    {"Accueil", "Home", 0, icons::Id::Home},
    {"Instances", "Instances", 1, icons::Id::Instances},
    {"Jouer", "Play", 2, icons::Id::Play},
    {"Serveurs", "Servers", 3, icons::Id::Servers},
    {"Skins", "Skins", 4, icons::Id::Skins},
    {"Actualités", "News", 5, icons::Id::News},
    {"Bedrock", "Bedrock", 6, icons::Id::Bedrock},
    {"Compte", "Account", 7, icons::Id::Account},
    {"Paramètres", "Settings", 8, icons::Id::Settings},
    {"Exploration", "Explore", 9, icons::Id::Explore},
    {"Explorateur de fichiers", "File explorer", 10, icons::Id::Files},
    {"Édition de carte", "Map editor", 11, icons::Id::Map},
    {"Générateur de ville OSM", "OSM city generator", 12, icons::Id::City},
    {"Développement de mods", "Mod development", 13, icons::Id::ModDev},
    {"Visualiseur de modèles 3D", "3D model viewer", 14, icons::Id::Model},
    {"Téléchargements", "Downloads", 15, icons::Id::Download},
};

std::vector<Hit> collect(const std::string& q) {
    std::vector<Hit> out;

    for (const auto& p : kPages) {
        const char* label = tr(p.fr, p.en);
        const int sc = subseq_score(label, q);
        if (sc < 0) continue;
        Hit h;
        h.label = label;
        h.hint = tr("Page", "Page");
        h.icon = p.icon;
        h.page = p.page;
        h.score = sc;
        out.push_back(std::move(h));
    }

    auto& arr = inst_array();
    if (arr.is_array())
        for (auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string name = e.value("Name", "");
            if (name.empty()) continue;
            int sc = subseq_score(name, q);
            // Les tags comptent aussi : chercher « pvp » doit ramener
            // l'instance marquee ainsi, comme dans la liste.
            if (sc < 0)
                if (const auto it = e.find("Tags");
                    it != e.end() && it->is_array())
                    for (const auto& t : *it)
                        if (t.is_string()) {
                            const int ts = subseq_score(t.get<std::string>(), q);
                            if (ts > sc) sc = ts;
                        }
            if (sc < 0) continue;
            Hit h;
            h.label = name;
            h.hint = e.value("Loader", "") + " · " + e.value("McVersion", "");
            h.icon = icons::Id::Instances;
            h.page = 1;
            h.instId = e.value("Id", "");
            // Les favoris remontent, comme dans la liste d'instances.
            h.score = sc + (e.value("Favorite", false) ? 5 : 0);
            out.push_back(std::move(h));
        }

    std::stable_sort(out.begin(), out.end(),
                     [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (out.size() > 12) out.resize(12); // au-dela, la liste n'aide plus
    return out;
}

} // namespace

void search_open() {
    S.open = true;
    S.focus = true;
    S.sel = 0;
    S.query[0] = '\0';
}

void search_frame() {
    // TL_AUTO_PALETTE=<texte> : ouvre la palette avec cette recherche, sans
    // clavier (capture et test). La palette s'ouvrant sur Ctrl+K, elle
    // serait sinon invisible aux captures automatiques.
    static bool paletteAuto = false;
    if (!paletteAuto) {
        paletteAuto = true;
        if (const char* q = std::getenv("TL_AUTO_PALETTE")) {
            search_open();
            std::snprintf(S.query, sizeof(S.query), "%s", q);
        }
    }

    // Ctrl+K depuis n'importe ou. `false` : pas de repetition automatique si
    // la touche reste enfoncee.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) search_open();
    if (!S.open) return;

    const ImVec2 disp = io.DisplaySize;
    const float w = (std::min)(560.0f, disp.x - 80.0f);
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.18f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f));
    ImGui::OpenPopup("###globalsearch");

    if (!ImGui::BeginPopupModal("###globalsearch", nullptr,
                                ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_AlwaysAutoResize))
        return;

    if (S.focus) {
        ImGui::SetKeyboardFocusHere();
        S.focus = false;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::SetNextItemWidth(-1.0f);
    const bool submitted = ImGui::InputTextWithHint(
        "##gsq", tr("Instance, page, réglage…", "Instance, page, setting…"),
        S.query, sizeof(S.query), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();

    const auto hits = collect(S.query);
    if (S.sel >= static_cast<int>(hits.size()))
        S.sel = hits.empty() ? 0 : static_cast<int>(hits.size()) - 1;
    if (S.sel < 0) S.sel = 0;

    // Navigation au clavier : la souris ne doit jamais etre necessaire.
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && !hits.empty())
        S.sel = (S.sel + 1) % static_cast<int>(hits.size());
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && !hits.empty())
        S.sel = (S.sel + static_cast<int>(hits.size()) - 1) %
                static_cast<int>(hits.size());

    auto activate = [&](const Hit& h) {
        if (!h.instId.empty()) g.selInstId = h.instId;
        if (h.page >= 0) g.page = h.page;
        S.open = false;
        ImGui::CloseCurrentPopup();
    };

    ImGui::Spacing();
    if (hits.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(S.query[0] == '\0'
                                   ? tr("Tape pour chercher.", "Type to search.")
                                   : tr("Aucun résultat.", "No match."));
        ImGui::PopStyleColor();
    } else {
        for (int i = 0; i < static_cast<int>(hits.size()); ++i) {
            const Hit& h = hits[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rowH = ImGui::GetFontSize() + 14.0f;
            if (ImGui::Selectable("##row", i == S.sel, 0, ImVec2(0, rowH)))
                activate(h);
            // Le survol deplace la selection : clavier et souris restent
            // d'accord sur ce qui est « courant ».
            if (ImGui::IsItemHovered()) S.sel = i;

            const float icoS = ImGui::GetFontSize();
            icons::draw(ImGui::GetWindowDrawList(), h.icon,
                        ImVec2(p.x + 6, p.y + (rowH - icoS) * 0.5f), icoS,
                        ImGui::ColorConvertFloat4ToU32(kText));
            ImGui::SetCursorScreenPos(
                ImVec2(p.x + icoS + 16, p.y + (rowH - ImGui::GetFontSize()) * 0.5f));
            ImGui::TextUnformatted(h.label.c_str());
            if (!h.hint.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(h.hint.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
            ImGui::PopID();
        }
    }

    if (submitted && !hits.empty()) activate(hits[static_cast<std::size_t>(S.sel)]);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        S.open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(
        tr("↑ ↓ pour naviguer · Entrée pour ouvrir · Échap pour fermer",
           "Up/Down to move - Enter to open - Esc to close"));
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

} // namespace tl::ui
