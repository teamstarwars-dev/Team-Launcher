#include "ui_internal.hpp"

#include "modmeta.hpp"

#include <algorithm>
#include <map>
#include <thread>

// ---------------------------------------------------------------------------
// Phase 5 — comparateur de modpacks.
//
// « Pourquoi ça marche sur mon autre instance ? » est la question la plus
// frequente quand on bricole ses mods. Comparer deux dossiers a la main,
// quarante jars de chaque cote, avec des noms de fichiers qui ne se
// ressemblent pas, est penible et faux : c'est l'identifiant DECLARE par
// le mod qui compte, pas le nom du fichier.
//
// On compare donc par modId, et on rapporte trois choses : ce qui n'est
// que d'un cote, ce qui est des deux mais dans une version differente, et
// ce qui est identique. Purement local, aucune requete.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct Row {
    std::string id;
    std::string name;
    std::string leftVer;   // vide = absent a gauche
    std::string rightVer;  // vide = absent a droite
    bool leftDisabled = false;
    bool rightDisabled = false;
};

struct Cmp {
    std::mutex m;
    std::thread th;
    bool busy = false;
    bool done = false;
    std::string leftId, rightId;
    std::string leftName, rightName;
    std::vector<Row> rows;
    int onlyLeft = 0, onlyRight = 0, differ = 0, same = 0;
};
Cmp c;

bool open_ = false;
bool request_ = false;
int filter_ = 0; // 0 tout, 1 differences seulement

void reap() {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(c.m);
        if (c.busy || !c.th.joinable()) return;
        th = std::move(c.th);
    }
    th.join();
}

void start(const std::string& leftId, const std::string& leftName,
           const std::string& rightId, const std::string& rightName) {
    {
        std::lock_guard<std::mutex> lk(c.m);
        if (c.busy) return;
        c.busy = true;
        c.done = false;
        c.rows.clear();
    }
    reap();
    const auto root = DataStore::instancesRoot();
    std::lock_guard<std::mutex> lk(c.m);
    c.th = std::thread([root, leftId, leftName, rightId, rightName] {
        const auto L = modmeta::read_dir(root / leftId / "mods");
        const auto R = modmeta::read_dir(root / rightId / "mods");

        // Cle : le modId quand il est lisible, sinon le nom de fichier —
        // un jar dont on ne sait pas lire le manifeste doit quand meme
        // apparaitre dans la comparaison, pas disparaitre en silence.
        auto key = [](const modmeta::Mod& m) {
            return m.id.empty() ? "?" + m.file : m.id;
        };
        std::map<std::string, Row> rows;
        // Un meme modId peut venir de DEUX jars du meme cote (c'est le
        // conflit que modcheck signale). Ecraser la premiere version par la
        // seconde afficherait tranquillement la mauvaise : on les accole,
        // et le doublon se voit.
        auto put = [](std::string& slot, const std::string& v) {
            const std::string val = v.empty() ? "?" : v;
            if (slot.empty()) slot = val;
            else if (slot.find(val) == std::string::npos) slot += " + " + val;
        };
        for (const auto& m : L) {
            auto& r = rows[key(m)];
            r.id = key(m);
            if (r.name.empty()) r.name = m.name;
            put(r.leftVer, m.version);
            r.leftDisabled = r.leftDisabled || m.disabled;
        }
        for (const auto& m : R) {
            auto& r = rows[key(m)];
            r.id = key(m);
            if (r.name.empty()) r.name = m.name;
            put(r.rightVer, m.version);
            r.rightDisabled = r.rightDisabled || m.disabled;
        }

        std::vector<Row> out;
        int onlyL = 0, onlyR = 0, diff = 0, eq = 0;
        out.reserve(rows.size());
        for (auto& [k, r] : rows) {
            if (r.leftVer.empty()) ++onlyR;
            else if (r.rightVer.empty()) ++onlyL;
            else if (r.leftVer != r.rightVer) ++diff;
            else ++eq;
            out.push_back(std::move(r));
        }
        // Les differences d'abord : c'est ce qu'on est venu voir.
        std::stable_sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
            auto rank = [](const Row& x) {
                if (x.leftVer.empty() || x.rightVer.empty()) return 0;
                if (x.leftVer != x.rightVer) return 1;
                return 2;
            };
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            return a.name < b.name;
        });

        std::lock_guard<std::mutex> lk2(c.m);
        c.busy = false;
        c.done = true;
        c.leftId = leftId;
        c.rightId = rightId;
        c.leftName = leftName;
        c.rightName = rightName;
        c.rows = std::move(out);
        c.onlyLeft = onlyL;
        c.onlyRight = onlyR;
        c.differ = diff;
        c.same = eq;
    });
}

} // namespace

void compare_open(const std::string& leftId) {
    std::lock_guard<std::mutex> lk(c.m);
    c.leftId = leftId;
    const nlohmann::json* e = find_instance(leftId);
    c.leftName = e ? e->value("Name", leftId) : leftId;
    c.rightId.clear();
    c.done = false;
    c.rows.clear();
    request_ = true;
}

void compare_modal() {
    reap();
    if (request_) {
        request_ = false;
        open_ = true;
        ImGui::OpenPopup("##cmpmods");
    }
    if (!open_) return;
    ImGui::SetNextWindowSize(ImVec2(760, 540), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##cmpmods", nullptr,
                                ImGuiWindowFlags_NoSavedSettings))
        return;

    std::string leftId, leftName, rightId, rightName;
    bool busy, done;
    int onlyL, onlyR, diff, eq;
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lk(c.m);
        leftId = c.leftId;
        leftName = c.leftName;
        rightId = c.rightId;
        rightName = c.rightName;
        busy = c.busy;
        done = c.done;
        rows = c.rows;
        onlyL = c.onlyLeft;
        onlyR = c.onlyRight;
        diff = c.differ;
        eq = c.same;
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Comparer les mods", "Compare mods"));
    if (fBig) ImGui::PopFont();

    // Choix de l'instance de droite.
    ImGui::TextUnformatted(leftName.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(tr("comparée à", "compared with"));
    ImGui::SameLine();
    {
        std::vector<std::string> ids, labels;
        for (const auto& e : inst_array()) {
            if (!e.is_object()) continue;
            const std::string id = e.value("Id", "");
            if (id.empty() || id == leftId) continue; // pas elle-meme
            ids.push_back(id);
            labels.push_back(e.value("Name", id));
        }
        if (ids.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr("(aucune autre instance)",
                                      "(no other instance)"));
            ImGui::PopStyleColor();
        } else {
            int idx = 0;
            for (size_t i = 0; i < ids.size(); ++i)
                if (ids[i] == rightId) idx = static_cast<int>(i);
            std::vector<const char*> items;
            for (const auto& l : labels) items.push_back(l.c_str());
            ImGui::SetNextItemWidth(240.0f);
            const bool changed = ImGui::Combo("##cmpright", &idx, items.data(),
                                              static_cast<int>(items.size()));
            // Premiere ouverture : on compare tout de suite avec la
            // premiere instance de la liste, sans exiger un clic de plus.
            if (changed || (rightId.empty() && !busy))
                start(leftId, leftName, ids[static_cast<size_t>(idx)],
                      labels[static_cast<size_t>(idx)]);
        }
    }

    ImGui::Separator();
    if (busy || !done) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Lecture des mods...", "Reading mods..."));
        ImGui::PopStyleColor();
        ImGui::EndPopup();
        return;
    }

    ImGui::Text(tr("%d seulement à gauche, %d seulement à droite, "
                   "%d en version différente, %d identique(s)",
                   "%d only on the left, %d only on the right, "
                   "%d in a different version, %d identical"),
                onlyL, onlyR, diff, eq);
    ImGui::RadioButton(tr("Tout", "All"), &filter_, 0);
    ImGui::SameLine();
    ImGui::RadioButton(tr("Différences seulement", "Differences only"),
                       &filter_, 1);

    ImGui::BeginChild("##cmpbody", ImVec2(0, -44));
    if (ImGui::BeginTable("##cmptable", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(tr("Mod"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(leftName.c_str(),
                                ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(rightName.empty() ? "?" : rightName.c_str(),
                                ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            const bool identical = !r.leftVer.empty() && !r.rightVer.empty() &&
                                   r.leftVer == r.rightVer;
            if (filter_ == 1 && identical) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.name.c_str());

            auto cell = [&](const std::string& ver, bool disabled,
                            bool absentHere) {
                if (absentHere) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    ImGui::TextUnformatted(tr("absent", "missing"));
                    ImGui::PopStyleColor();
                    return;
                }
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      identical ? kDim : hex(0xE0A030));
                ImGui::TextUnformatted(ver.c_str());
                ImGui::PopStyleColor();
                if (disabled) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    ImGui::TextUnformatted(tr("(désactivé)", "(disabled)"));
                    ImGui::PopStyleColor();
                }
            };
            ImGui::TableSetColumnIndex(1);
            cell(r.leftVer, r.leftDisabled, r.leftVer.empty());
            ImGui::TableSetColumnIndex(2);
            cell(r.rightVer, r.rightDisabled, r.rightVer.empty());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (accent_button(tr("Fermer"), ImVec2(140, 34))) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void compare_stop() {
    reap();
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(c.m);
        th = std::move(c.th);
    }
    if (th.joinable()) th.join();
}

} // namespace tl::ui
