#include "downloads.hpp"

#include "datastore.hpp"
#include "http_win.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace tl::downloads {

namespace {

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Etat d'un element cote moteur. `cancel` est un shared_ptr : le travailleur
// garde une reference pendant tout le telechargement, meme si l'element est
// relance entre-temps (la relance cree un NOUVEAU drapeau, l'ancien reste
// valide pour le travailleur qui s'arrete).
struct Entry {
    Item item;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::string rawUrl; // URL non redigee : l'interface ne la voit jamais
};

struct Engine {
    std::mutex m;
    std::condition_variable cv;      // reveille les travailleurs
    std::condition_variable cvIdle;  // reveille wait_idle
    std::map<int, Entry> items;      // par identifiant
    std::deque<int> queue;           // identifiants en attente
    int nextId = 1;
    int running = 0;
    int maxParallel = 4;
    std::uint64_t ver = 0;
    bool stopping = false;
    std::vector<std::thread> workers;
    Fetcher fetcher;
};

Engine& eng() {
    static Engine e;
    return e;
}

// Transport par defaut : le client HTTP du launcher, avec son allowlist et
// sa redaction. Aucun contournement possible depuis ce module.
bool default_fetch(const std::string& url, const fs::path& dest,
                   const ProgressFn& progress, const std::atomic<bool>& cancel,
                   std::string* errOut) {
    const bool ok = http::get_to_file(
        url, dest,
        [&](long long done, long long total) {
            if (progress) progress(done, total);
        },
        &cancel);
    if (!ok && errOut)
        *errOut = cancel.load() ? "Annule." : "Telechargement echoue.";
    return ok;
}

void bump(Engine& e) {
    ++e.ver;
}

// Combien de travailleurs devraient tourner, compte tenu de la limite et de
// ce qui reste en file. On n'en cree jamais plus que necessaire : un modpack
// de 3 fichiers n'a pas besoin de 20 threads.
void ensure_workers(Engine& e);
// Definie plus bas (elle a besoin des helpers de serialisation), mais
// appelee des le worker : declaration anticipee.
void save_locked(Engine& e);

void worker_loop() {
    Engine& e = eng();
    for (;;) {
        int id = 0;
        Fetcher fetch;
        std::string url;
        fs::path dest;
        std::shared_ptr<std::atomic<bool>> cancel;
        {
            std::unique_lock<std::mutex> lk(e.m);
            e.cv.wait(lk, [&] {
                return e.stopping ||
                       (!e.queue.empty() && e.running < e.maxParallel);
            });
            if (e.stopping) return;
            id = e.queue.front();
            e.queue.pop_front();
            auto it = e.items.find(id);
            if (it == e.items.end()) continue; // retire entre-temps
            // Annule pendant l'attente : on ne le lance pas.
            if (it->second.item.state != State::Queued) continue;
            it->second.item.state = State::Running;
            it->second.item.speedBps = 0.0;
            fetch = e.fetcher;
            url = it->second.rawUrl;
            dest = it->second.item.dest;
            cancel = it->second.cancel;
            ++e.running;
            bump(e);
        }

        // --- hors verrou : le telechargement peut durer des minutes ---
        const auto t0 = std::chrono::steady_clock::now();
        long long lastBytes = 0;
        auto lastTick = t0;
        std::string err;

        auto progress = [&](long long done, long long total) {
            const auto now = std::chrono::steady_clock::now();
            const double dt =
                std::chrono::duration<double>(now - lastTick).count();
            // Vitesse recalculee au plus deux fois par seconde : sinon la
            // valeur saute a chaque paquet et devient illisible.
            std::lock_guard<std::mutex> lk(e.m);
            auto it = e.items.find(id);
            if (it == e.items.end()) return;
            it->second.item.done = done;
            it->second.item.total = total;
            if (dt >= 0.5) {
                const double inst = static_cast<double>(done - lastBytes) / dt;
                // Moyenne glissante : amortit les a-coups du reseau.
                it->second.item.speedBps =
                    it->second.item.speedBps <= 0.0
                        ? inst
                        : it->second.item.speedBps * 0.7 + inst * 0.3;
                lastBytes = done;
                lastTick = now;
            }
            bump(e);
        };

        const bool ok = fetch ? fetch(url, dest, progress, *cancel, &err) : false;

        {
            std::lock_guard<std::mutex> lk(e.m);
            auto it = e.items.find(id);
            // Fermeture du launcher : `shutdown()` a leve le drapeau
            // d'annulation de TOUT le monde. Ce n'est pas une annulation de
            // l'utilisateur, c'est une interruption — l'element doit rester
            // a reprendre au prochain demarrage, pas etre classe abandonne.
            // Sans cette distinction, quitter pendant un telechargement le
            // perdait definitivement (constate en test bout en bout).
            const bool interrupted = e.stopping;
            if (it != e.items.end() && !interrupted) {
                Item& x = it->second.item;
                if (cancel->load()) {
                    x.state = State::Cancelled;
                    if (x.error.empty()) x.error = "Annule.";
                } else if (ok) {
                    x.state = State::Done;
                    if (x.total > 0) x.done = x.total;
                    x.error.clear();
                } else {
                    x.state = State::Failed;
                    x.error = err.empty() ? "Telechargement echoue." : err;
                }
                x.endedUnix = now_unix();
                x.speedBps = 0.0;
            }
            --e.running;
            bump(e);
            // Pendant la fermeture on n'ecrit plus : l'instantane pris juste
            // avant `shutdown()` est le bon, le reecrire l'ecraserait.
            if (!interrupted) save_locked(e);
            e.cvIdle.notify_all();
        }
        e.cv.notify_all();
    }
}

void ensure_workers(Engine& e) {
    const std::size_t want =
        (std::min)(static_cast<std::size_t>(e.maxParallel),
                   e.queue.size() + static_cast<std::size_t>(e.running));
    while (e.workers.size() < want && e.workers.size() < 20)
        e.workers.emplace_back(worker_loop);
}

} // namespace

// --- Persistance ------------------------------------------------------------

fs::path state_path() { return DataStore::dir() / "downloads.json"; }

namespace {

// Au-dela, l'historique des termines n'apporte plus rien et le fichier
// grossit sans fin.
constexpr std::size_t kKeepFinished = 50;

const char* state_name(State s) {
    switch (s) {
        case State::Queued: return "queued";
        case State::Running: return "queued"; // interrompu -> a refaire
        case State::Done: return "done";
        case State::Failed: return "failed";
        case State::Cancelled: return "cancelled";
    }
    return "queued";
}

State state_from(const std::string& s) {
    if (s == "done") return State::Done;
    if (s == "failed") return State::Failed;
    if (s == "cancelled") return State::Cancelled;
    return State::Queued;
}

} // namespace

namespace {

// Version interne : le verrou est DEJA tenu par l'appelant. Ecrire depuis
// l'interieur des sections critiques evite la danse « relacher, ecrire,
// reprendre », qui laisserait la file changer entre-temps et permettrait
// deux ecritures concurrentes dans le desordre. Le fichier fait quelques
// kilooctets, le verrou n'est donc tenu qu'un instant de plus.
void save_locked(Engine& e) {
    nlohmann::json arr = nlohmann::json::array();
    std::size_t finished = 0;
    {
        // Du plus recent au plus ancien : on garde les N derniers termines.
        std::vector<const Entry*> ents;
        for (const auto& [id, en] : e.items) ents.push_back(&en);
        std::sort(ents.begin(), ents.end(), [](const Entry* a, const Entry* b) {
            return a->item.id > b->item.id;
        });
        for (const Entry* en : ents) {
            const Item& x = en->item;
            if (!x.active()) {
                if (++finished > kKeepFinished) continue;
            }
            // Une URL porteuse de secret n'est jamais ecrite sur le disque.
            // `redact_url` est deja la reference pour « qu'est-ce qui est
            // sensible » : on s'en sert comme predicat plutot que de
            // maintenir une seconde liste qui divergerait.
            if (http::redact_url(en->rawUrl) != en->rawUrl) continue;
            arr.push_back({{"label", x.label},
                           {"url", en->rawUrl},
                           {"dest", x.dest.string()},
                           {"state", state_name(x.state)},
                           {"total", x.total},
                           {"error", x.error},
                           {"queued", x.queuedUnix},
                           {"ended", x.endedUnix}});
        }
    }

    std::error_code ec;
    const fs::path p = state_path();
    fs::create_directories(p.parent_path(), ec);
    // Temporaire + remplacement : une coupure ne doit pas laisser un JSON
    // tronque, qui ferait perdre TOUTE la file au prochain demarrage.
    const fs::path tmp = p.string() + ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << arr.dump(1);
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
}

} // namespace

void save_state() {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    save_locked(e);
}

void load_state() {
    std::error_code ec;
    const fs::path p = state_path();
    if (!fs::is_regular_file(p, ec)) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const auto arr = nlohmann::json::parse(text, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return;

    Engine& e = eng();
    std::vector<int> toStart;
    {
        std::lock_guard<std::mutex> lk(e.m);
        if (!e.fetcher) e.fetcher = default_fetch;
        // Ordre chronologique : l'ancien fichier est du plus recent au plus
        // ancien, on le remet a l'endroit pour conserver l'ordre de file.
        for (auto it = arr.rbegin(); it != arr.rend(); ++it) {
            const auto& j = *it;
            if (!j.is_object()) continue;
            const std::string url = j.value("url", "");
            const std::string dest = j.value("dest", "");
            if (url.empty() || dest.empty()) continue;

            Entry en;
            en.rawUrl = url;
            en.cancel = std::make_shared<std::atomic<bool>>(false);
            en.item.id = e.nextId++;
            en.item.label = j.value("label", "");
            if (en.item.label.empty())
                en.item.label = fs::path(dest).filename().string();
            en.item.url = http::redact_url(url);
            en.item.dest = fs::path(dest);
            en.item.total = j.value("total", -1LL);
            en.item.state = state_from(j.value("state", "queued"));
            en.item.error = j.value("error", "");
            en.item.queuedUnix = j.value("queued", 0LL);
            en.item.endedUnix = j.value("ended", 0LL);

            // Un element termine dont le fichier a disparu depuis n'a plus
            // de sens dans l'historique.
            if (en.item.state == State::Done) {
                std::error_code e2;
                if (!fs::exists(en.item.dest, e2)) continue;
            }
            const int id = en.item.id;
            const bool requeue = en.item.state == State::Queued;
            e.items.emplace(id, std::move(en));
            if (requeue) {
                e.queue.push_back(id);
                toStart.push_back(id);
            }
        }
        bump(e);
        ensure_workers(e);
    }
    if (!toStart.empty()) e.cv.notify_all();
}

int enqueue(const std::string& label, const std::string& url,
            const fs::path& dest) {
    if (url.empty() || dest.empty()) return 0;
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    if (!e.fetcher) e.fetcher = default_fetch;

    Entry en;
    en.rawUrl = url;
    en.cancel = std::make_shared<std::atomic<bool>>(false);
    en.item.id = e.nextId++;
    en.item.label = label.empty() ? dest.filename().string() : label;
    // L'URL stockee pour l'affichage est REDIGEE : un jeton de telechargement
    // ne doit pas se retrouver dans une capture d'ecran ou un journal.
    en.item.url = http::redact_url(url);
    en.item.dest = dest;
    en.item.queuedUnix = now_unix();

    const int id = en.item.id;
    e.items.emplace(id, std::move(en));
    e.queue.push_back(id);
    bump(e);
    save_locked(e);
    ensure_workers(e);
    e.cv.notify_one();
    return id;
}

std::vector<Item> snapshot() {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    std::vector<Item> out;
    out.reserve(e.items.size());
    for (const auto& [id, en] : e.items) out.push_back(en.item);
    // Du plus recent au plus ancien : ce qu'on vient de lancer est en haut.
    std::sort(out.begin(), out.end(),
              [](const Item& a, const Item& b) { return a.id > b.id; });
    return out;
}

std::size_t count_active() {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    std::size_t n = 0;
    for (const auto& [id, en] : e.items)
        if (en.item.active()) ++n;
    return n;
}

std::size_t count_finished() {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    std::size_t n = 0;
    for (const auto& [id, en] : e.items)
        if (!en.item.active()) ++n;
    return n;
}

bool cancel(int id) {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    auto it = e.items.find(id);
    if (it == e.items.end()) return false;
    Item& x = it->second.item;
    if (!x.active()) return false;
    it->second.cancel->store(true);
    if (x.state == State::Queued) {
        // Pas encore parti : on le marque tout de suite, le travailleur le
        // sautera en trouvant un etat different de Queued.
        x.state = State::Cancelled;
        x.error = "Annule.";
        x.endedUnix = now_unix();
        e.cvIdle.notify_all();
    }
    bump(e);
    save_locked(e);
    return true;
}

void cancel_all() {
    Engine& e = eng();
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(e.m);
        for (const auto& [id, en] : e.items)
            if (en.item.active()) ids.push_back(id);
    }
    for (int id : ids) cancel(id);
}

bool retry(int id) {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    auto it = e.items.find(id);
    if (it == e.items.end()) return false;
    Item& x = it->second.item;
    if (x.state != State::Failed && x.state != State::Cancelled) return false;
    // Drapeau NEUF : l'ancien peut encore etre observe par un travailleur
    // qui n'a pas fini de se retirer.
    it->second.cancel = std::make_shared<std::atomic<bool>>(false);
    x.state = State::Queued;
    x.done = 0;
    x.speedBps = 0.0;
    x.error.clear();
    x.endedUnix = 0;
    x.queuedUnix = now_unix();
    e.queue.push_back(id);
    bump(e);
    save_locked(e);
    ensure_workers(e);
    e.cv.notify_one();
    return true;
}

void clear_finished() {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    for (auto it = e.items.begin(); it != e.items.end();)
        it = it->second.item.active() ? std::next(it) : e.items.erase(it);
    bump(e);
    save_locked(e);
}

void set_limit(int n) {
    Engine& e = eng();
    {
        std::lock_guard<std::mutex> lk(e.m);
        e.maxParallel = std::clamp(n, 1, 20);
        ensure_workers(e);
    }
    // Relever la limite doit debloquer les travailleurs en attente.
    e.cv.notify_all();
}

int limit() {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    return e.maxParallel;
}

std::uint64_t version() {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    return e.ver;
}

bool wait_idle(int timeoutMs) {
    Engine& e = eng();
    std::unique_lock<std::mutex> lk(e.m);
    auto idle = [&] { return e.queue.empty() && e.running == 0; };
    if (timeoutMs < 0) {
        e.cvIdle.wait(lk, idle);
        return true;
    }
    return e.cvIdle.wait_for(lk, std::chrono::milliseconds(timeoutMs), idle);
}

void shutdown() {
    Engine& e = eng();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lk(e.m);
        e.stopping = true;
        for (auto& [id, en] : e.items)
            if (en.item.active()) en.cancel->store(true);
        workers.swap(e.workers);
    }
    e.cv.notify_all();
    for (auto& t : workers)
        if (t.joinable()) t.join();
}

void set_fetcher(Fetcher f) {
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    e.fetcher = f ? std::move(f) : Fetcher(default_fetch);
}

void reset_for_tests() {
    shutdown();
    Engine& e = eng();
    std::lock_guard<std::mutex> lk(e.m);
    e.items.clear();
    e.queue.clear();
    e.nextId = 1;
    e.running = 0;
    e.maxParallel = 4;
    e.ver = 0;
    e.stopping = false;
    e.fetcher = default_fetch;
}

} // namespace tl::downloads
