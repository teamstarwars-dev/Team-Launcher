#include "ui_internal.hpp"

#include "http_win.hpp"
#include "skin_service.hpp"
#include "util_image.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#endif

#include <cmath>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <unordered_set>

namespace tl::ui {

// ---------------------------------------------------------------------------
// Page Skins (portage de SkinsPage.cs + SkinCatalogService.cs + SkinTools.cs) :
// apercu 360 px a gauche, bibliotheque a droite (scroll), worker asynchrone
// pour les telechargements (thumbs, imports, catalogue, application).
// ---------------------------------------------------------------------------

namespace {

// Cette page avait sa PROPRE palette sombre, figee a l'initialisation
// statique. Elle serait donc restee noire en theme clair, et ignorait le
// mode daltonisme et les couleurs personnalisees. Les neuf teintes suivent
// maintenant la palette globale.
//
// Ce sont des fonctions et non des constantes : la palette change a chaud
// quand on bascule de theme, une valeur figee au demarrage ne suivrait pas.
inline ImVec4 kSkinBg() { return kBg; }
inline ImVec4 kSkinCard() { return kCard; }
inline ImVec4 kSkinHover() { return shade_by(kCard, 0.05f); }
inline ImVec4 kSkinAccent() { return kAccent; }
inline ImVec4 kSkinText() { return kText; }
inline ImVec4 kSkinDim() { return kDim; }
inline ImVec4 kSkinDanger() { return kDanger; }
inline ImVec4 kSkinBtn() { return kButton; }
inline ImVec4 kSkinBtnOn() { return kButtonActive; }
const ImVec4 kWhite = ImVec4(1, 1, 1, 1);

struct OnlineSkin {
    std::string name, download, preview, author, skinId;
};

struct ThumbJob {
    std::string key, url, name;
};
struct ThumbRes {
    std::string key, bytes;
    bool ok = false;
};

enum class TaskKind { Apply, ImportName, ImportUrl, FetchMine, OnlineImport, Catalog };
struct Task {
    TaskKind kind = TaskKind::Apply;
    std::string a, b;               // selon kind
    std::string name;               // OnlineImport (pseudo affiche)
    std::vector<nlohmann::json> instances; // Apply
    std::string playerName;         // Apply
    // Annulation propre a la tache : liee a son entree AppTasks (le panneau
    // n'annule qu'elle, pas tout le worker). Posee par push_task.
    std::shared_ptr<std::atomic<bool>> cancel;
    int apptaskId = 0;              // entree AppTasks (0 = aucune)
};
struct TaskResult {
    std::string status;
    bool toast = false;
    std::string toastTitle, toastMsg;
    bool rebuild = false;
    std::string select;
    bool setTab = false;
    int tab = 0;
    bool catalog = false;
    std::vector<OnlineSkin> online;
    bool failed = false; // echec reel -> « Echouee » dans le panneau
};

struct SkinsSt {
    bool inited = false;
    int tab = 0; // 0 Tous, 1 Officiels, 2 Favoris, 3 En ligne
    char searchBuf[128] = "";
    std::string search;
    std::vector<std::string> view; // fichiers filtrés
    std::string selected;
    std::vector<std::string> favs;
    bool favsLoaded = false;
    bool libDirty = true;
    std::string status;

    // aperçu (main thread, texture GL)
    std::string previewPath;
    unsigned previewTex = 0;
    int previewW = 0, previewH = 0;
    bool previewTried = false;
    float zoom = 1.0f, rot = 0.0f;

    // vignettes (key -> texture, 0 = échec)
    std::map<std::string, unsigned> thumbTex;
    std::set<std::string> thumbPending;

    int modal = 0; // 1 pseudo, 2 url
    bool modalRequest = false;
    char modalBuf[512] = "";

    std::vector<OnlineSkin> online;
    bool loadingOnline = false;

    // worker
    bool workerStarted = false;
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    std::atomic<bool> cancel{false};
    // Drapeau de la tache en vol (sous m) : skins_stop() le leve aussi, sinon
    // http ne verrait que le drapeau propre a la tache et ignorerait l'arret.
    std::atomic<bool>* runningCancel = nullptr;
    std::deque<ThumbJob> thumbQ;
    std::deque<Task> taskQ;
    std::deque<ThumbRes> thumbRes;
    std::deque<TaskResult> taskRes;
};

SkinsSt S;

// --- helpers texte ---------------------------------------------------------

std::string lower_copy(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ci_eq(const std::string& a, const std::string& b) {
    return lower_copy(a) == lower_copy(b);
}

bool ci_contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return lower_copy(hay).find(lower_copy(needle)) != std::string::npos;
}

std::string trim_copy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

// Uri.EscapeDataString (C#) : unreserved seulement.
std::string url_esc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            o.push_back(static_cast<char>(c));
        else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            o += b;
        }
    }
    return o;
}

bool valid_mc_name(const std::string& s) {
    if (s.size() < 3 || s.size() > 16) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    return true;
}

// Ellipsis ASCII (« ... » hors plage Latin-1 de la police par defaut).
std::string ellipsis(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string o = s;
    while (!o.empty() &&
           ImGui::CalcTextSize((o + "...").c_str()).x > maxW)
        o.pop_back();
    return o + "...";
}

std::string stem_of(const std::string& p) {
    return std::filesystem::path(p).stem().string();
}

bool looks_like_image(const std::string& bytes) {
    if (bytes.size() < 4) return false;
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return true;
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;
    return false;
}

std::string b64_decode(const std::string& in) {
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> rev(256, -1);
    for (int i = 0; i < 64; i++)
        rev[static_cast<unsigned char>(tbl[i])] = i;
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (rev[c] < 0) continue;
        val = (val << 6) + rev[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Wrap horizontal (remplace le WrapPanel C#) : cartes 100x128 + marge 10.
struct CardFlow {
    float startX, x, y, rowH = 0, maxRight, spacing;
    explicit CardFlow(float spacing = 10.0f)
        : startX(ImGui::GetCursorPosX()), x(startX),
          y(ImGui::GetCursorPosY()),
          maxRight(startX + ImGui::GetContentRegionAvail().x),
          spacing(spacing) {}
    void slot(float w) {
        if (x > startX && x + w > maxRight) {
            x = startX;
            y += rowH + spacing;
            rowH = 0;
        }
        ImGui::SetCursorPos(ImVec2(x, y));
    }
    void advance(float w, float h) {
        x += w + spacing;
        rowH = (std::max)(rowH, h);
    }
    void end() { ImGui::SetCursorPos(ImVec2(startX, y + rowH)); }
};

// --- favoris (skins-favorites.json, meme format C# : liste de pseudos) ----

std::filesystem::path fav_path() {
    return DataStore::dir() / "skins-favorites.json";
}

void load_favs() {
    S.favsLoaded = true;
    S.favs.clear();
    std::error_code ec;
    if (!std::filesystem::exists(fav_path(), ec)) return;
    std::ifstream in(fav_path());
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (j.is_array())
            for (const auto& e : j)
                if (e.is_string()) S.favs.push_back(e.get<std::string>());
    } catch (...) {}
}

void save_favs() {
    try {
        std::error_code ec;
        std::filesystem::create_directories(fav_path().parent_path(), ec);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& f : S.favs) arr.push_back(f);
        std::ofstream ofs(fav_path(), std::ios::trunc);
        ofs << arr.dump();
    } catch (...) {}
}

bool is_fav(const std::string& name) {
    for (const auto& f : S.favs)
        if (f == name) return true;
    return false;
}

// --- worker ----------------------------------------------------------------

TaskResult run_task(const Task& t);
ThumbRes run_thumb(const ThumbJob& j);
std::vector<OnlineSkin> catalog_fetch(const std::string& query,
                                      std::string& status,
                                      const std::atomic<bool>* cancel);

// Titre affiche dans le panneau de taches. Compose ici (fil UI) : la page
// n'affiche pas de titre, le panneau traduit a l'affichage.
std::string task_title(const Task& t) {
    switch (t.kind) {
    case TaskKind::Apply:
        return tr("Application du skin", "Apply skin");
    case TaskKind::ImportName:
        return std::string(tr("Import de skin : ", "Skin import: ")) + t.a;
    case TaskKind::ImportUrl:
        return tr("Import de skin (URL)", "Skin import (URL)");
    case TaskKind::FetchMine:
        return std::string(tr("Récupération du skin : ", "Skin fetch: ")) + t.a;
    case TaskKind::OnlineImport:
        return std::string(tr("Ajout du skin : ", "Add skin: ")) +
               (t.name.empty() ? t.a : t.name);
    case TaskKind::Catalog:
        return tr("Chargement du catalogue", "Loading catalog");
    }
    return tr("Tâche skins", "Skins task");
}

// Rapport de fin vers le panneau. Appele PAR LE WORKER, pendant que `t` est
// encore vivant (son drapeau reste referencable jusqu'a apptasks_end).
void end_task_entry(const Task& t, const TaskResult& r) {
    if (t.apptaskId <= 0) return;
    // Annulation locale non relayee (arret de la page) -> « Annulee ».
    if (t.cancel && t.cancel->load()) (void)tl::tasks::cancel(t.apptaskId);
    if (r.failed) {
        apptasks_end(t.apptaskId, r.status);
    } else {
        tl::tasks::update(t.apptaskId, r.status);
        apptasks_end(t.apptaskId);
    }
}

void ensure_worker() {
    if (S.workerStarted) return;
    S.workerStarted = true;
    S.th = std::thread([] {
        for (;;) {
            Task task;
            ThumbJob tj;
            bool haveTask = false, haveThumb = false;
            {
                std::unique_lock<std::mutex> lk(S.m);
                S.cv.wait(lk, [] {
                    return S.cancel.load() || !S.taskQ.empty() ||
                           !S.thumbQ.empty();
                });
                if (S.cancel.load()) return;
                if (!S.taskQ.empty()) {
                    task = std::move(S.taskQ.front());
                    S.taskQ.pop_front();
                    // Publie sous verrou AVANT execution : un arret (qui prend
                    // egalement ce verrou) ne peut pas manquer le drapeau.
                    S.runningCancel =
                        task.cancel ? task.cancel.get() : &S.cancel;
                    haveTask = true;
                } else if (!S.thumbQ.empty()) {
                    tj = std::move(S.thumbQ.front());
                    S.thumbQ.pop_front();
                    haveThumb = true;
                }
            }
            if (haveTask) {
                TaskResult r = run_task(task);
                // Fin de tache tant que `task` (et son drapeau) est en vie :
                // le lien vers le registre est retire ici.
                end_task_entry(task, r);
                std::lock_guard<std::mutex> lk(S.m);
                S.runningCancel = nullptr;
                S.taskRes.push_back(std::move(r));
            } else if (haveThumb) {
                ThumbRes r = run_thumb(tj);
                std::lock_guard<std::mutex> lk(S.m);
                S.thumbRes.push_back(std::move(r));
            }
        }
    });
}

void push_task(Task t) {
    // Suivi comme tache de fond : annulation par tache (le panneau n'arrete
    // pas tout le worker). Le pointeur reste valide : il est porte par le
    // shared_ptr de la tache, de la file jusqu'a la fin de run_task.
    t.cancel = std::make_shared<std::atomic<bool>>(false);
    t.apptaskId = apptasks_begin(task_title(t), "", t.cancel.get());
    {
        std::lock_guard<std::mutex> lk(S.m);
        S.taskQ.push_back(std::move(t));
    }
    ensure_worker();
    S.cv.notify_one();
}

void request_thumb(const std::string& key, const std::string& url,
                   const std::string& name) {
    std::lock_guard<std::mutex> lk(S.m);
    if (S.thumbTex.count(key) || S.thumbPending.count(key)) return;
    if (S.thumbQ.size() > 192) return;
    S.thumbPending.insert(key);
    S.thumbQ.push_back({key, url, name});
    ensure_worker();
    S.cv.notify_one();
}

// Telechargement d'une vignette : url -> fichier local -> mc-heads avatar.
ThumbRes run_thumb(const ThumbJob& j) {
    ThumbRes r;
    r.key = j.key;
    const std::atomic<bool>& cancel = S.cancel;
    if (!j.url.empty()) {
        auto resp = http::get_string(j.url, &cancel);
        if (resp && looks_like_image(*resp)) {
            r.bytes = *resp;
            r.ok = true;
            return r;
        }
    }
    if (!j.name.empty()) {
        std::error_code ec;
        const auto local = DataStore::skinsDir() / (j.name + ".png");
        if (std::filesystem::exists(local, ec)) {
            std::ifstream f(local, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            if (looks_like_image(bytes)) {
                r.bytes = std::move(bytes);
                r.ok = true;
                return r;
            }
        }
        if (j.url.find("mc-heads.net/avatar/") == std::string::npos) {
            auto resp = http::get_string(
                "https://mc-heads.net/avatar/" + url_esc(j.name) + "/64",
                &cancel);
            if (resp && looks_like_image(*resp)) {
                r.bytes = *resp;
                r.ok = true;
            }
        }
    }
    return r;
}

// --- taches (worker) : imports, application, catalogue ---------------------

TaskResult run_task(const Task& t) {
    TaskResult r;
    // Annulation propre a la tache (panneau) ; repli sur l'arret global.
    const std::atomic<bool>* cancel =
        t.cancel ? t.cancel.get() : &S.cancel;
    try {
        switch (t.kind) {
        case TaskKind::Apply: {
            int ok = 0;
            std::vector<std::string> errors;
            for (const auto& inst : t.instances) {
                try {
                    tl::skin::apply(inst, t.a, t.playerName, cancel);
                    ++ok;
                } catch (const std::exception& e) {
                    errors.push_back(inst.value("Name", "?") + ": " + e.what());
                }
            }
            const std::string base = "Appliqué à " + std::to_string(ok);
            if (errors.empty()) {
                r.status = base + " instance(s).";
            } else {
                std::string joined;
                for (size_t i = 0; i < errors.size() && i < 3; i++) {
                    if (i) joined += ", ";
                    joined += errors[i];
                }
                r.status = base + ". Erreurs : " + joined;
            }
            break;
        }
        case TaskKind::ImportName: {
            std::error_code ec;
            std::filesystem::create_directories(DataStore::skinsDir(), ec);
            const std::string dest =
                (DataStore::skinsDir() / (t.a + ".png")).string();
            if (tl::skin::download_by_name(t.a, dest, cancel)) {
                r.status = "Skin de " + t.a + " importé.";
                r.rebuild = true;
                r.select = dest;
                r.setTab = true;
                r.tab = 0;
            } else {
                r.status = "Erreur : telechargement impossible.";
                r.failed = true;
            }
            break;
        }
        case TaskKind::ImportUrl: {
            if (http::get_to_file(t.a, t.b, nullptr, cancel)) {
                r.status = "Importé : " + std::filesystem::path(t.b).filename().string();
                r.rebuild = true;
                r.select = t.b;
                r.setTab = true;
                r.tab = 0;
            } else {
                r.status = "Erreur : telechargement impossible.";
                r.failed = true;
            }
            break;
        }
        case TaskKind::FetchMine: {
            if (tl::skin::download_by_name(t.a, t.b, cancel)) {
                r.status = "Skin officiel de " + t.a + " récupéré.";
                r.rebuild = true;
                r.select = t.b;
                r.setTab = true;
                r.tab = 1;
            } else {
                r.status = "Skin introuvable pour ce pseudo.";
                r.failed = true;
            }
            break;
        }
        case TaskKind::OnlineImport: {
            if (http::get_to_file(t.a, t.b, nullptr, cancel)) {
                r.status = t.name + " ajouté à la bibliothèque.";
                r.rebuild = true;
                r.select = t.b;
                r.setTab = true;
                r.tab = 0;
            } else {
                r.status = "Erreur : telechargement impossible.";
                r.failed = true;
            }
            break;
        }
        case TaskKind::Catalog: {
            r.catalog = true;
            r.online = catalog_fetch(t.a, r.status, cancel);
            break;
        }
        }
    } catch (const std::exception& e) {
        r.status = std::string("Erreur : ") + e.what();
    } catch (...) {
        r.status = "Erreur : echec inattendu.";
    }
    return r;
}

// --- catalogue en ligne (SkinCatalogService.cs) ----------------------------

OnlineSkin player_skin(const std::string& name, const std::string& author) {
    OnlineSkin s;
    s.name = name;
    s.download = "https://mc-heads.net/skin/" + url_esc(name);
    s.preview = "https://mc-heads.net/avatar/" + url_esc(name) + "/96";
    s.author = author;
    return s;
}

const char* const kPopular[] = {
    "Dream",        "GeorgeNotFound", "Sapnap",   "TommyInnit", "Tubbo",
    "Ranboo",       "Technoblade",    "Notch",    "jeb_",       "Dinnerbone",
    "Grumm",        "MHF_Steve",      "Alex",     "Herobrine",  "xNestorio",
    "Illumina",     "Purpled",        "F1NN5TER", "Skeppy",     "Zelk",
    "WadZee",       "TimeDeo",        "MegaPvP934", "Pat_Box",  "sensism",
    "nvura",        "hideyourscars",  "kobosanger", "khoats",   "ImSanemi",
    "coldified",    "SkinEditor",     "DamianGrr",  "MrCraft72"};

// Quirk C# conserve : un skin sans SkinId est toujours rejete (le test
// « doublon de pseudo » revoit la cle qui vient d'etre inseree).
void add_range(std::vector<OnlineSkin>& target,
               std::unordered_set<std::string>& seen,
               const std::vector<OnlineSkin>& items) {
    for (const auto& s : items) {
        const std::string key = s.skinId.empty() ? "n:" + lower_copy(s.name)
                                                 : "id:" + s.skinId;
        if (!seen.insert(key).second) continue;
        if (s.skinId.empty() && seen.count("n:" + lower_copy(s.name))) continue;
        target.push_back(s);
        if (target.size() >= 48) return;
    }
}

std::string online_status(const std::vector<OnlineSkin>& out) {
    for (const auto& s : out)
        if (s.author.find("archive") != std::string::npos)
            return std::to_string(out.size()) +
                   " skins NameMC (archive Wayback - live bloqué). "
                   "Clique pour importer.";
    return std::to_string(out.size()) + " skin(s) en ligne. Clique pour importer.";
}

// HTML NameMC : paires skin-id/pseudo (36 max), repli sur /profile/<pseudo>.
std::vector<OnlineSkin> parse_namemc(const std::string& html, bool fromArchive) {
    std::vector<OnlineSkin> list;
    std::unordered_set<std::string> seen;
    const std::string author = fromArchive ? "NameMC (archive)" : "NameMC";
    static const std::regex rxPair(
        R"re(href="[^"]*/skin/([0-9a-f]{16})"[^>]*>\s*<div class="card-header[^"]*"[^>]*>\s*<span[^>]*>([^<]+)</span>)re",
        std::regex::ECMAScript | std::regex::icase);
    try {
        for (std::sregex_iterator it(html.begin(), html.end(), rxPair), end;
             it != end; ++it) {
            const std::string id = (*it)[1].str();
            const std::string name = trim_copy((*it)[2].str());
            if (!valid_mc_name(name)) continue;
            if (!seen.insert("id:" + id).second) continue;
            OnlineSkin s;
            s.name = name;
            s.download = "https://mc-heads.net/skin/" + url_esc(name);
            s.preview =
                "https://s.namemc.com/2d/skin/face.png?id=" + id + "&scale=4";
            s.author = author;
            s.skinId = id;
            list.push_back(std::move(s));
            if (list.size() >= 36) break;
        }
    } catch (...) {}
    if (list.empty()) {
        static const std::regex rxProf(R"(/profile/([a-zA-Z0-9_]{3,16}))");
        try {
            for (std::sregex_iterator it(html.begin(), html.end(), rxProf), end;
                 it != end; ++it) {
                const std::string name = (*it)[1].str();
                if (!seen.insert("n:" + lower_copy(name)).second) continue;
                list.push_back(player_skin(name, author));
                if (list.size() >= 36) break;
            }
        } catch (...) {}
    }
    return list;
}

// Derniere capture Wayback du trending NameMC (API puis URL de repli).
std::vector<OnlineSkin> fetch_wayback(const std::atomic<bool>& cancel) {
    std::string snapUrl;
    const auto resp = http::get_string(
        "https://archive.org/wayback/available?url=" +
            url_esc("namemc.com/minecraft-skins/trending"),
        &cancel);
    if (resp) {
        try {
            const auto doc = nlohmann::json::parse(*resp, nullptr, false);
            if (doc.is_object())
                snapUrl = doc.value("archived_snapshots", nlohmann::json::object())
                              .value("closest", nlohmann::json::object())
                              .value("url", "");
        } catch (...) {}
    }
    if (snapUrl.empty())
        snapUrl =
            "http://web.archive.org/web/20260723040516/"
            "https://namemc.com/minecraft-skins/trending";
    const auto page = http::get_string(snapUrl, &cancel);
    if (!page) throw std::runtime_error("archive Wayback indisponible");
    return parse_namemc(*page, true);
}

// cool-skins (SkinsRestorer) : textures Mojang en base64.
std::vector<OnlineSkin> fetch_cool(const std::atomic<bool>& cancel) {
    std::vector<OnlineSkin> list;
    const auto resp = http::get_string(
        "https://raw.githubusercontent.com/SkinsRestorer/cool-skins/"
        "main/list.json",
        &cancel);
    if (!resp) throw std::runtime_error("cool-skins indisponible");
    const auto doc = nlohmann::json::parse(*resp, nullptr, false);
    if (!doc.is_object()) return list;
    const auto& skins = doc.value("skins", nlohmann::json::array());
    if (!skins.is_array()) return list;
    static const std::regex rxUrl(R"url("url"\s*:\s*"([^"]+)")url");
    for (const auto& s : skins) {
        const std::string skinName = s.value("skinName", "");
        const std::string valueB64 = s.value("value", "");
        if (skinName.empty() || valueB64.empty()) continue;
        std::string textureUrl;
        try {
            const std::string decoded = b64_decode(valueB64);
            std::smatch m;
            if (std::regex_search(decoded, m, rxUrl) && m.size() > 1) {
                textureUrl = m[1].str();
                const std::string httpPrefix = "http://";
                if (textureUrl.rfind(httpPrefix, 0) == 0)
                    textureUrl = "https://" + textureUrl.substr(httpPrefix.size());
            }
        } catch (...) {}
        if (textureUrl.empty()) continue;
        OnlineSkin entry;
        entry.name = skinName;
        entry.download = textureUrl;
        entry.preview = valid_mc_name(skinName)
                            ? "https://mc-heads.net/avatar/" + url_esc(skinName) + "/96"
                            : textureUrl;
        entry.author = "Cool Skins";
        list.push_back(std::move(entry));
        if (list.size() >= 36) break;
    }
    return list;
}

// Etapes : Wayback (live omis : Cloudflare 403 constate) -> cool-skins ->
// 34 pseudos populaires embarques.
std::vector<OnlineSkin> fetch_trending(const std::atomic<bool>& cancel,
                                       std::string& status) {
    std::vector<OnlineSkin> out;
    std::unordered_set<std::string> seen;
    try {
        add_range(out, seen, fetch_wayback(cancel));
        if (out.size() >= 24) {
            status = online_status(out);
            return out;
        }
    } catch (...) {}
    try {
        add_range(out, seen, fetch_cool(cancel));
    } catch (...) {}
    for (const char* n : kPopular) {
        if (!seen.insert(std::string(n)).second) continue;
        out.push_back(player_skin(n, "Populaire"));
        if (out.size() >= 40) break;
    }
    status = online_status(out);
    return out;
}

// Recherche par pseudo (Mojang) ou tendances.
std::vector<OnlineSkin> catalog_fetch(const std::string& query,
                                      std::string& status,
                                      const std::atomic<bool>* cancel) {
    // Le jeton de la tache prime sur celui de la page : annuler une recherche
    // ne doit pas arreter les autres travaux du worker.
    const std::atomic<bool>& stop = cancel ? *cancel : S.cancel;
    std::vector<OnlineSkin> out;
    const std::string q = trim_copy(query);
    if (!q.empty()) {
        if (!valid_mc_name(q)) {
            status = "Pseudo invalide (3-16 caractères, lettres/chiffres/_).";
            return out;
        }
        const auto resp = http::get_string(
            "https://api.mojang.com/users/profiles/minecraft/" + url_esc(q),
            &stop);
        bool found = false;
        if (resp) {
            try {
                const auto doc = nlohmann::json::parse(*resp, nullptr, false);
                found = doc.is_object() && doc.contains("id");
            } catch (...) {}
        }
        if (!found) {
            status = "Aucun joueur trouvé pour « " + q + " ».";
            return out;
        }
        out.push_back(player_skin(q, "Recherche"));
        status = "1 skin(s) en ligne. Clique pour importer.";
        return out;
    }
    out = fetch_trending(stop, status);
    return out;
}

// --- bibliotheque locale (main thread) -------------------------------------

void rebuild_library() {
    std::error_code ec;
    const auto dir = DataStore::skinsDir();
    std::filesystem::create_directories(dir, ec);
    std::vector<std::string> files;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        if (!ci_eq(it->path().extension().string(), ".png")) continue;
        files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end(),
              [](const std::string& a, const std::string& b) {
                  return CaseInsensitiveLess{}(a, b);
              });

    // recherche (sur le nom de fichier, comme C#)
    std::vector<std::string> out;
    for (const auto& f : files)
        if (ci_contains(stem_of(f), S.search)) out.push_back(f);

    // onglets Officiels / Favoris
    if (S.tab == 1) {
        std::vector<std::string> f2;
        for (const auto& f : out)
            if (ci_eq(stem_of(f), DataStore::settings.playerName))
                f2.push_back(f);
        out.swap(f2);
    } else if (S.tab == 2) {
        std::vector<std::string> f2;
        for (const auto& f : out)
            if (is_fav(stem_of(f))) f2.push_back(f);
        out.swap(f2);
    }

    // selection par defaut : skin officiel, sinon le premier filtre
    if (S.selected.empty() || !std::filesystem::exists(S.selected, ec)) {
        const auto official =
            dir / (DataStore::settings.playerName + ".png");
        if (std::filesystem::exists(official, ec))
            S.selected = official.string();
        else if (!out.empty())
            S.selected = out.front();
        else
            S.selected.clear();
    }
    S.view = std::move(out);
}

// Recharge la texture d'apercu quand la selection change (zoom/rotation remis).
void sync_preview() {
    std::error_code ec;
    std::string want;
    if (!S.selected.empty() && std::filesystem::exists(S.selected, ec))
        want = S.selected;
    if (want == S.previewPath && S.previewTried) return;
    if (S.previewTex) image::free_tex(S.previewTex);
    S.previewTex = 0;
    S.previewPath = want;
    S.previewTried = true;
    S.previewW = S.previewH = 0;
    S.zoom = 1.0f;
    S.rot = 0.0f;
    if (want.empty()) return;
    int w = 0, h = 0;
    S.previewTex = image::from_path(want, &w, &h);
    S.previewW = w;
    S.previewH = h;
}

// Resultats worker -> etat UI (main thread).
void drain_results() {
    std::deque<ThumbRes> hr;
    std::deque<TaskResult> taskRes;
    {
        std::lock_guard<std::mutex> lk(S.m);
        hr.swap(S.thumbRes);
        taskRes.swap(S.taskRes);
    }
    for (auto& r : hr) {
        S.thumbPending.erase(r.key);
        unsigned tex = 0;
        if (r.ok)
            tex = image::from_mem(r.bytes.data(),
                                  static_cast<int>(r.bytes.size()));
        auto it = S.thumbTex.find(r.key);
        if (it != S.thumbTex.end() && it->second) image::free_tex(it->second);
        S.thumbTex[r.key] = tex;
    }
    for (auto& r : taskRes) {
        if (!r.status.empty()) S.status = r.status;
        if (r.toast) notify_toast(r.toastTitle, r.toastMsg);
        if (r.rebuild) S.libDirty = true;
        if (r.catalog) {
            S.online = std::move(r.online);
            S.loadingOnline = false;
        }
        if (!r.select.empty()) S.selected = r.select;
        if (r.setTab) S.tab = r.tab;
    }
}

void start_catalog(const std::string& query) {
    if (S.loadingOnline) return;
    S.loadingOnline = true;
    S.online.clear();
    S.status = "Chargement du catalogue NameMC...";
    Task t;
    t.kind = TaskKind::Catalog;
    t.a = query;
    push_task(std::move(t));
}

// --- boites de dialogue (Win32 natif ; générique zenity/kdialog sous Linux) ---

std::vector<std::string> pick_png_open_multi() {
#ifdef _WIN32
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrFilter = L"Images PNG (*.png)\0*.png\0Tous les fichiers\0*.*\0";
    ofn.lpstrTitle = L"Importer un skin";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    std::vector<std::string> out;
    const wchar_t* p = buf.data();
    const std::wstring first(p);
    if (first.empty()) return out;
    p += first.size() + 1;
    if (*p == L'\0') { // fichier unique
        out.push_back(wstr_to_utf8(first.c_str()));
        return out;
    }
    while (*p != L'\0') {
        const std::wstring f(p);
        p += f.size() + 1;
        out.push_back(wstr_to_utf8((first + L"\\" + f).c_str()));
    }
    return out;
#else
    return pick_files_open("Importer un skin", "Images PNG (*.png)", "*.png");
#endif
}

std::optional<std::string> pick_png_save(const std::string& defaultName) {
#ifdef _WIN32
    wchar_t file[MAX_PATH] = L"";
    MultiByteToWideChar(CP_UTF8, 0, defaultName.c_str(), -1, file, MAX_PATH);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Image PNG (*.png)\0*.png\0";
    ofn.lpstrTitle = L"Exporter le skin";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
#else
    return pick_file_save("Exporter le skin", "Image PNG (*.png)", "*.png",
                          defaultName, "png");
#endif
}

// --- actions ---------------------------------------------------------------

void import_from_file() {
    const auto files = pick_png_open_multi();
    if (files.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(DataStore::skinsDir(), ec);
    int count = 0;
    for (const auto& f : files) {
        const std::filesystem::path src(f);
        const auto dest = DataStore::skinsDir() / src.filename();
        std::error_code ec2;
        if (std::filesystem::equivalent(src, dest, ec2)) {
            ++count; // deja dans le dossier
            continue;
        }
        std::filesystem::copy_file(
            src, dest, std::filesystem::copy_options::overwrite_existing, ec2);
        if (!ec2) ++count;
    }
    if (count == 0) {
        S.status = "Erreur : copie impossible.";
        return;
    }
    S.selected =
        (DataStore::skinsDir() / std::filesystem::path(files.front()).filename())
            .string();
    S.tab = 0;
    S.libDirty = true;
    S.status = std::to_string(count) + " skin(s) importé(s).";
}

void do_import_name(const std::string& raw) {
    const std::string n = trim_copy(raw);
    if (n.empty()) return;
    S.status = "Téléchargement du skin de " + n + "...";
    Task t;
    t.kind = TaskKind::ImportName;
    t.a = n;
    push_task(std::move(t));
}

void do_import_url(const std::string& raw) {
    const std::string u = trim_copy(raw);
    if (u.size() < 4 || lower_copy(u.substr(0, 4)) != "http") return;
    // dernier segment du chemin (query ignoree)
    const size_t q = u.find('?');
    const std::string path = (q == std::string::npos) ? u : u.substr(0, q);
    const size_t slash = path.find_last_of('/');
    std::string fileName =
        (slash == std::string::npos) ? std::string() : path.substr(slash + 1);
    if (fileName.size() < 4 || !ci_eq(fileName.substr(fileName.size() - 4), ".png")) {
        char b[32];
        std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        std::snprintf(b, sizeof(b), "skin_%02d%02d%02d.png", tmv.tm_hour,
                      tmv.tm_min, tmv.tm_sec);
        fileName = b;
    }
    S.status = "Téléchargement...";
    Task t;
    t.kind = TaskKind::ImportUrl;
    t.a = u;
    t.b = (DataStore::skinsDir() / fileName).string();
    push_task(std::move(t));
}

void fetch_mine() {
    const std::string name = trim_copy(DataStore::settings.playerName);
    if (name.empty()) {
        S.status = "Aucun pseudo configuré (Compte - Microsoft).";
        return;
    }
    S.status = "Récupération de ton skin officiel...";
    std::error_code ec;
    std::filesystem::create_directories(DataStore::skinsDir(), ec);
    const auto dest = DataStore::skinsDir() / (name + ".png");
    std::filesystem::remove(dest, ec); // force le re-telechargement (C#)
    Task t;
    t.kind = TaskKind::FetchMine;
    t.a = name;
    t.b = dest.string();
    push_task(std::move(t));
}

void apply_selected() {
    std::error_code ec;
    if (S.selected.empty() || !std::filesystem::exists(S.selected, ec)) {
        S.status = "Sélectionne d'abord un skin.";
        return;
    }
    S.status = "Application...";
    Task t;
    t.kind = TaskKind::Apply;
    t.a = S.selected;
    t.instances = DataStore::settings.instances;
    t.playerName = DataStore::settings.playerName;
    push_task(std::move(t));
}

void toggle_favorite() {
    std::error_code ec;
    if (S.selected.empty() || !std::filesystem::exists(S.selected, ec)) return;
    const std::string name = stem_of(S.selected);
    const auto it = std::find(S.favs.begin(), S.favs.end(), name);
    if (it != S.favs.end())
        S.favs.erase(it);
    else
        S.favs.push_back(name);
    save_favs();
    S.libDirty = true;
}

void export_selected() {
    std::error_code ec;
    if (S.selected.empty() || !std::filesystem::exists(S.selected, ec)) return;
    const auto dst =
        pick_png_save(std::filesystem::path(S.selected).filename().string());
    if (!dst) return;
    std::error_code ec2;
    std::filesystem::copy_file(S.selected, *dst,
                               std::filesystem::copy_options::overwrite_existing,
                               ec2);
    if (ec2) {
        S.status = "Erreur : " + ec2.message();
        return;
    }
    S.status = "Skin exporté.";
}

void delete_selected() {
    std::error_code ec;
    if (S.selected.empty() || !std::filesystem::exists(S.selected, ec)) return;
    const std::string name = stem_of(S.selected);
    std::filesystem::remove(S.selected, ec);
    S.favs.erase(std::remove(S.favs.begin(), S.favs.end(), name), S.favs.end());
    save_favs();
    S.selected.clear();
    S.libDirty = true;
    S.status = "Skin supprimé.";
}

void refresh_library() {
    for (auto& kv : S.thumbTex)
        if (kv.second) image::free_tex(kv.second);
    S.thumbTex.clear();
    S.libDirty = true;
}

void open_folder() {
    std::error_code ec;
    std::filesystem::create_directories(DataStore::skinsDir(), ec);
    open_in_explorer(DataStore::skinsDir());
}

void download_online(const OnlineSkin& s) {
    S.status = "Import de " + s.name + "...";
    std::error_code ec;
    std::filesystem::create_directories(DataStore::skinsDir(), ec);
    Task t;
    t.kind = TaskKind::OnlineImport;
    t.a = s.download;
    t.b = (DataStore::skinsDir() / (s.name + ".png")).string();
    t.name = s.name;
    push_task(std::move(t));
}

// --- widgets ---------------------------------------------------------------

bool ghost_btn(const char* label, const ImVec4& col) {
    ImGui::PushStyleColor(ImGuiCol_Button, kSkinBtn());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSkinHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSkinBtnOn());
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    const bool r = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return r;
}

bool action_btn(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, kSkinBtn());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSkinHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSkinBtnOn());
    const bool r = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return r;
}

bool accent_skin_btn(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kSkinAccent());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(0x8fb4f9));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hex(0x6a92e8));
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

bool tab_btn(const char* label, bool active) {
    ImGui::PushStyleColor(ImGuiCol_Button, active ? kSkinHover() : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? kSkinHover() : kSkinBtn());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSkinBtnOn());
    ImGui::PushStyleColor(ImGuiCol_Text, active ? kSkinAccent() : kSkinDim());
    const bool r = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return r;
}

void set_tab(int t) {
    if (S.tab == t) return;
    S.tab = t;
    S.libDirty = true;
    if (t == 3 && S.online.empty()) start_catalog("");
}

// Coeur dessine (U+2665 hors plage Latin-1 de la police par defaut).
void draw_heart(ImDrawList* dl, float cx, float cy, float s, ImU32 col) {    dl->AddCircleFilled(ImVec2(cx - 0.24f * s, cy - 0.10f * s), 0.24f * s, col,
                        16);
    dl->AddCircleFilled(ImVec2(cx + 0.24f * s, cy - 0.10f * s), 0.24f * s, col,
                        16);
    dl->AddTriangleFilled(ImVec2(cx - 0.46f * s, cy - 0.02f * s),
                          ImVec2(cx + 0.46f * s, cy - 0.02f * s),
                          ImVec2(cx, cy + 0.50f * s), col);
}

// Texture GL -> ImTextureID (binaire compile avec ImTextureID = ImU64).
ImTextureID tex_id(unsigned tex) {
    return static_cast<ImTextureID>(static_cast<uintptr_t>(tex));
}

// Vignette carree : texture si chargee, sinon carré + initiale du pseudo.
void draw_thumb(ImDrawList* dl, const ImVec2& t0, float size,
                const std::string& key, const std::string& url,
                const std::string& name) {
    request_thumb(key, url, name);
    const ImVec2 t1(t0.x + size, t0.y + size);
    const auto it = S.thumbTex.find(key);
    if (it != S.thumbTex.end() && it->second) {
        dl->AddImage(tex_id(it->second), t0, t1);
        return;
    }
    dl->AddRectFilled(t0, t1, ImGui::ColorConvertFloat4ToU32(kSkinHover()), 6.0f);
    const std::string ini =
        name.empty() ? "?" : std::string(1, static_cast<char>(
                                               std::toupper(static_cast<unsigned char>(name[0]))));
    const ImVec2 ts = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2((t0.x + t1.x - ts.x) * 0.5f, (t0.y + t1.y - ts.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(kSkinDim()), ini.c_str());
}

void open_modal(int kind) {
    S.modal = kind;
    S.modalRequest = true;
}

// Carte locale : clic = selection, double-clic = application (C# Tapped/DoubleTapped).
void card_local(const std::string& path, int idx) {
    const std::string name = stem_of(path);
    const bool sel = ci_eq(path, S.selected);
    ImGui::PushID(idx);
    ImGui::InvisibleButton("##c", ImVec2(100, 128));
    const bool hov = ImGui::IsItemHovered();
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) S.selected = path;
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        S.selected = path;
        apply_selected();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
        sel ? kSkinAccent() : (hov ? kSkinHover() : kSkinCard()));
    dl->AddRectFilled(p0, p1, bg, 10.0f);

    const bool fav = is_fav(name);
    const float contentH =
        56.0f + 6.0f + 14.0f + (fav ? 6.0f + 12.0f : 0.0f);
    float y = p0.y + (128.0f - contentH) * 0.5f;
    const float cx = (p0.x + p1.x) * 0.5f;
    draw_thumb(dl, ImVec2(cx - 28.0f, y), 56.0f, "h:" + lower_copy(name),
               "https://mc-heads.net/avatar/" + url_esc(name) + "/64", name);
    y += 56.0f + 6.0f;
    const std::string label = ellipsis(name, 84.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(cx - ts.x * 0.5f, y),
                ImGui::ColorConvertFloat4ToU32(sel ? kWhite : kSkinText()),
                label.c_str());
    if (fav) {
        y += 14.0f + 6.0f;
        draw_heart(dl, cx, y + 5.0f, 12.0f,
                   ImGui::ColorConvertFloat4ToU32(kSkinDanger()));
    }
    ImGui::PopID();
}

// Carte en ligne : clic = telechargement dans la bibliotheque.
void card_online(const OnlineSkin& s, int idx) {
    ImGui::PushID(idx);
    ImGui::InvisibleButton("##o", ImVec2(100, 128));
    const bool hov = ImGui::IsItemHovered();
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) download_online(s);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    dl->AddRectFilled(p0, p1,
                      ImGui::ColorConvertFloat4ToU32(hov ? kSkinHover() : kSkinCard()),
                      10.0f);
    const float y = p0.y + (128.0f - (56.0f + 6.0f + 14.0f)) * 0.5f;
    const float cx = (p0.x + p1.x) * 0.5f;
    const std::string key = s.preview.empty()
                                ? "h:" + lower_copy(s.name)
                                : "u:" + s.preview;
    draw_thumb(dl, ImVec2(cx - 28.0f, y), 56.0f, key, s.preview, s.name);
    const std::string label = ellipsis(s.name, 84.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(cx - ts.x * 0.5f, y + 56.0f + 6.0f),
                ImGui::ColorConvertFloat4ToU32(kSkinText()), label.c_str());
    ImGui::PopID();
}

// --- colonne gauche : apercu ------------------------------------------------

std::string account_label() {
    const std::string mode = DataStore::settings.accountMode;
    const std::string name = DataStore::settings.playerName;
    if (mode == "microsoft" && !trim_copy(name).empty())
        return "Connecté : " + name + " - le skin s'applique au lancement.";
    if (mode == "offline" && !trim_copy(name).empty())
        return "Hors-ligne : " + name +
               " - applique le skin via CustomSkinLoader.";
    return "Aucun compte Microsoft : connectez-vous pour appliquer un skin.";
}

void left_column() {
    const float availH = ImGui::GetContentRegionAvail().y;
    // Sous l'apercu : indice (20) + nom (26) + apply (44) + rangée (36) +
    // indice bas (52) — la boîte s'adapte pour que la colonne tienne sans
    // scroll (contrairement au ScrollViewer C#).
    const float rest = 20.0f + 26.0f + 44.0f + 36.0f + 52.0f;
    const float boxH = (std::clamp)(availH - rest, 260.0f, 420.0f);

    ImGui::BeginChild("##prev", ImVec2(0, boxH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b0 = ImGui::GetWindowPos();
    const ImVec2 b1(b0.x + ImGui::GetWindowSize().x,
                    b0.y + ImGui::GetWindowSize().y);
    dl->AddRectFilled(b0, b1, ImGui::ColorConvertFloat4ToU32(kSkinCard()), 12.0f);

    const bool has = S.previewTex != 0;
    static bool dragging = false;
    static ImVec2 lastDrag;
    const bool hov = has && ImGui::IsWindowHovered();
    if (hov) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            S.zoom = (std::clamp)(S.zoom * (1.0f + 0.15f * wheel), 0.4f, 3.0f);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if (!dragging) {
                dragging = true;
                lastDrag = mp;
            } else {
                S.rot += (mp.x - lastDrag.x) * 0.6f;
                lastDrag = mp;
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;

    if (has) {
        // Texture centrée, ratio conserve, rotation + zoom (C# TransformGroup).
        const float maxW = 180.0f * S.zoom;
        const float maxH = 320.0f * S.zoom;
        const float ar =
            (S.previewH > 0) ? static_cast<float>(S.previewW) /
                                   static_cast<float>(S.previewH)
                             : 1.0f;
        float w = maxW, h = w / ar;
        if (h > maxH) {
            h = maxH;
            w = h * ar;
        }
        const ImVec2 c((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f);
        const float rad = S.rot * 3.14159265f / 180.0f;
        const float cs = std::cos(rad), sn = std::sin(rad);
        const float hx = w * 0.5f, hy = h * 0.5f;
        const float dx[4] = {-hx, hx, hx, -hx};
        const float dy[4] = {-hy, -hy, hy, hy};
        ImVec2 p[4];
        for (int i = 0; i < 4; i++) {
            p[i].x = c.x + dx[i] * cs - dy[i] * sn;
            p[i].y = c.y + dx[i] * sn + dy[i] * cs;
        }
        dl->AddImageQuad(tex_id(S.previewTex), p[0], p[1], p[2], p[3],
                         ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                         ImVec2(0, 1));
    } else {
        const char* txt = tr("Aucun skin choisi");
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2((b0.x + b1.x - ts.x) * 0.5f,
                           (b0.y + b1.y - ts.y) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(kSkinDim()), txt);
    }
    ImGui::EndChild();

    // indice + Recadrer
    ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
    if (fSmall) ImGui::PushFont(fSmall);
    ImGui::TextUnformatted(tr("Glissez pour tourner - molette pour zoomer"));
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();
    {
        const float bw = ImGui::CalcTextSize(tr("Recadrer")).x + 20.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - bw - 4.0f);
        ImGui::BeginDisabled(!has);
        if (ghost_btn(tr("Recadrer"), kSkinText())) {
            S.zoom = 1.0f;
            S.rot = 0.0f;
        }
        ImGui::EndDisabled();
    }

    if (has) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kSkinText());
        ImGui::TextUnformatted(stem_of(S.selected).c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (accent_skin_btn(tr("Appliquer à toutes les instances"),
                            ImVec2(ImGui::GetContentRegionAvail().x, 36)))
            apply_selected();
        ImGui::Spacing();

        const bool fav = is_fav(stem_of(S.selected));
        if (ghost_btn(fav ? tr("Retirer favori") : tr("Favori"),
                      fav ? kSkinDanger() : kSkinText()))
            toggle_favorite();
        ImGui::SameLine();
        if (ghost_btn(tr("Exporter"), kSkinText())) export_selected();
        ImGui::SameLine();
        if (ghost_btn(tr("Supprimer"), kSkinDanger())) delete_selected();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
    if (has) {
        const std::string hint =
            tr("Skin sélectionné : ", "Selected skin: ") + stem_of(S.selected) +
            tr(". Applique-le à tes instances pour le voir en jeu.",
               ". Apply it to your instances to see it in-game.");
        ImGui::TextWrapped("%s", hint.c_str());
    } else {
        ImGui::TextWrapped(
            "%s", tr("Choisissez un skin dans la bibliothèque, ou importez-en un."));
    }
    ImGui::PopStyleColor();
}

// --- colonne droite : bibliotheque -----------------------------------------

void right_column() {
    // en-tête : titre + Actualiser / Dossier (glyphes C# hors police)
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Ma bibliothèque"));
    if (fBig) ImGui::PopFont();
    const float w1 = ImGui::CalcTextSize(tr("Actualiser")).x + 28.0f;
    const float w2 = ImGui::CalcTextSize(tr("Dossier")).x + 28.0f;
    const float rightEdge =
        ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize - 4.0f;
    ImGui::SameLine(rightEdge - w1 - w2 - 12.0f);
    if (action_btn(tr("Actualiser"))) refresh_library();
    ImGui::SameLine(rightEdge - w2);
    if (action_btn(tr("Dossier"))) open_folder();
    ImGui::Spacing();

    // actions d'import (WrapPanel C#)
    const char* const kActions[] = {
        tr("Depuis un fichier"),  tr("Depuis un pseudo"),
        tr("Depuis une adresse"), tr("Récupérer le mien"),
        tr("Catalogue en ligne"), tr("Ouvrir le dossier")};
    CardFlow aflow(8.0f);
    for (int i = 0; i < 6; i++) {
        const float w = ImGui::CalcTextSize(kActions[i]).x + 28.0f;
        aflow.slot(w);
        bool hit = action_btn(kActions[i]);
        aflow.advance(w, ImGui::GetFrameHeight());
        if (!hit) continue;
        switch (i) {
        case 0: import_from_file(); break;
        case 1: open_modal(1); break;
        case 2: open_modal(2); break;
        case 3: fetch_mine(); break;
        case 4:
            S.tab = 3;
            start_catalog("");
            break;
        default: open_folder(); break;
        }
    }
    aflow.end();

    // onglets
    ImGui::Spacing();
    if (tab_btn(tr("Tous"), S.tab == 0)) set_tab(0);
    ImGui::SameLine();
    if (tab_btn(tr("Officiels"), S.tab == 1)) set_tab(1);
    ImGui::SameLine();
    if (tab_btn(tr("Favoris"), S.tab == 2)) set_tab(2);
    ImGui::SameLine();
    if (tab_btn(tr("En ligne"), S.tab == 3)) set_tab(3);

    // recherche locale / pseudo en ligne (Enter)
    ImGui::Spacing();
    const bool online = S.tab == 3;
    if (ImGui::InputTextWithHint("##search",
                                 tr("Rechercher un skin ou un pseudo..."),
                                 S.searchBuf, sizeof(S.searchBuf))) {
        S.search = trim_copy(S.searchBuf);
        if (!online) S.libDirty = true;
    }
    const bool enterSearch =
        ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (online && enterSearch) start_catalog(trim_copy(S.searchBuf));

    // grille
    ImGui::Spacing();
    if (online && S.loadingOnline) {
        ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
        ImGui::TextUnformatted(tr("Chargement du catalogue en ligne..."));
        ImGui::PopStyleColor();
    } else if (online && S.online.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
        ImGui::TextWrapped(
            "%s", tr("Catalogue vide. Clique sur « Catalogue en ligne » ou cherche "
                     "un pseudo."));
        ImGui::PopStyleColor();
    } else if (!online && S.view.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
        if (S.tab == 2)
            ImGui::TextWrapped(
                "%s", tr("Aucun favori. Sélectionne un skin puis clique « Favori »."));
        else if (S.tab == 1)
            ImGui::TextWrapped(
                "%s", tr("Aucun skin officiel. Clique sur « Récupérer le mien »."));
        else
            ImGui::TextWrapped(
                "%s", tr("Votre bibliothèque est vide. Importez un skin depuis un "
                         "fichier, depuis le pseudo d'un joueur, ou depuis une "
                         "adresse."));
        ImGui::PopStyleColor();
    } else {
        CardFlow flow(10.0f);
        if (online) {
            for (size_t i = 0; i < S.online.size(); i++) {
                flow.slot(100.0f);
                card_online(S.online[i], static_cast<int>(i));
                flow.advance(100.0f, 128.0f);
            }
        } else {
            for (size_t i = 0; i < S.view.size(); i++) {
                flow.slot(100.0f);
                card_local(S.view[i], static_cast<int>(i));
                flow.advance(100.0f, 128.0f);
            }
        }
        flow.end();
    }

    if (!S.status.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
        if (fSmall) ImGui::PushFont(fSmall);
        // Traduction au moment de l'affichage : S.status est aussi ecrit par le
        // worker, qui ne doit pas lire la langue (course sur settings.language).
        const std::string st = tr(S.status);
        ImGui::TextUnformatted(st.c_str());
        if (fSmall) ImGui::PopFont();
        ImGui::PopStyleColor();
    }

    // COMPTE (comme Numek)
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
    if (fSmall) ImGui::PushFont(fSmall);
    ImGui::TextUnformatted(tr("COMPTE"));
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kSkinDim());
    ImGui::TextWrapped("%s", account_label().c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (accent_skin_btn(tr("Appliquer sur mon compte"), ImVec2(0, 0)))
        apply_selected();
}

// --- modales « Depuis un pseudo » / « Depuis une adresse » ------------------

void skins_modal() {
    static bool wasOpen = false;
    if (S.modalRequest) {
        S.modalRequest = false;
        S.modalBuf[0] = '\0';
        ImGui::OpenPopup("###skinimport");
    }
    const std::string title =
        std::string(S.modal == 1 ? tr("Depuis un pseudo") : tr("Depuis une adresse")) +
        "###skinimport";
    ImGui::SetNextWindowSize(ImVec2(420, 170), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(title.c_str(), &open,
                               ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize)) {
        wasOpen = true;
        ImGui::TextUnformatted(S.modal == 1 ? tr("Pseudo Minecraft :")
                                            : tr("URL du fichier .png :"));
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##in",
                                 S.modal == 1 ? "Steve" : "https://",
                                 S.modalBuf, sizeof(S.modalBuf));
        bool ok = ImGui::IsItemDeactivated() &&
                  ImGui::IsKeyPressed(ImGuiKey_Enter) && S.modalBuf[0] != '\0';
        ImGui::Spacing();
        const float bw = 120.0f;
        const float total = 12.0f + bw + 10.0f + bw;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                             ImGui::GetStyle().WindowPadding.x - total);
        if (ghost_btn(tr("Annuler"), kSkinText())) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (accent_skin_btn(tr("OK"), ImVec2(bw, 0))) ok = true;
        if (ok) {
            if (S.modal == 1)
                do_import_name(S.modalBuf);
            else
                do_import_url(S.modalBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (wasOpen) {
        wasOpen = false;
        S.modal = 0;
    }
}

} // namespace

void skins_page() {
    if (!S.inited) {
        S.inited = true;
        load_favs();
        // TL_AUTO_TAB (test) : ouvre directement l'onglet 0..3 (une seule fois)
        if (const char* t = std::getenv("TL_AUTO_TAB")) {
            const int i = std::atoi(t);
            if (i >= 0 && i <= 3) S.tab = i;
        }
    }

    drain_results();
    if (S.libDirty) {
        rebuild_library();
        S.libDirty = false;
    }
    sync_preview();
    if (S.tab == 3 && S.online.empty() && !S.loadingOnline) start_catalog("");

    // Marge C# (24, 20) — les enfants sans bordure n'ont pas de padding.
    ImGui::SetCursorPos(ImVec2(24.0f, 20.0f));

    ImGui::BeginChild("##skinleft", ImVec2(360, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    left_column();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 24.0f);

    ImGui::BeginChild("##skinright", ImVec2(0, 0));
    right_column();
    ImGui::EndChild();

    skins_modal();
}

void skins_stop() {
    S.cancel = true;
    S.cv.notify_all();
    if (S.th.joinable()) S.th.join();
    if (S.previewTex) {
        image::free_tex(S.previewTex);
        S.previewTex = 0;
    }
    for (auto& kv : S.thumbTex)
        if (kv.second) image::free_tex(kv.second);
    S.thumbTex.clear();
}

// Glisser-deposer global (DragDropHandler.cs) : skins pris en charge, les
// modpacks (.mrpack/.zip/.jar) le seront aux modules 4-5.
void skins_import_path(const char* path) {
    if (!path || !*path) return;
    const std::filesystem::path p(path);
    const std::string ext = lower_copy(p.extension().string());
    std::error_code ec;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
        std::filesystem::create_directories(DataStore::skinsDir(), ec);
        const auto dest = DataStore::skinsDir() / p.filename();
        std::error_code ec2;
        if (!std::filesystem::equivalent(p, dest, ec2)) {
            std::filesystem::copy_file(p, dest,
                                       std::filesystem::copy_options::
                                           overwrite_existing,
                                       ec2);
            if (ec2) {
                notify_toast(tr("Erreur d'import"), ec2.message());
                return;
            }
        }
        S.selected = dest.string();
        S.tab = 0;
        S.libDirty = true;
        notify_toast(tr("Skin importé"),
                     "« " + p.stem().string() + " » ajouté aux skins.");
        return;
    }
    // Modpacks (module 4d) : le format est détecté d'après le contenu.
    if (ext == ".mrpack" || ext == ".zip") {
        if (packs_busy()) {
            notify_toast(tr("Import en cours"),
                         tr("Attends la fin de l'import précédent.",
                            "Wait for the previous import to finish."));
            return;
        }
        g.page = 1; // page Instances : c'est là que s'affiche la progression
        import_modpack_start(path);
        return;
    }
    if (ext == ".jar") {
        notify_toast(tr("Import non porté"),
                     tr("Déposer un mod seul n'est pas encore géré.",
                        "Dropping a single mod is not supported yet."));
        return;
    }
    notify_toast(tr("Fichier non supporté"),
                 "Type " + ext + " non reconnu.");
}

} // namespace tl::ui
