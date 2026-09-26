#include <winsock2.h>
#include <ws2tcpip.h>

#include "ui_internal.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#pragma comment(lib, "ws2_32.lib")

namespace tl::ui {

// ---------------------------------------------------------------------------
// Page Serveurs (fidele ServersPage C#) : 3 onglets — Mes serveurs (cartes
// herbees, creation), Favoris (ping SLP TCP 1.7), Villes de la team.
// ServerHost/ServerPanel = module 4 : cartes en etat "Non installé", boutons
// Gérer/Démarrer/Arrêter desactives (tooltip « portage module 4 »).
// Presse-papiers Win32 (copie IP, Partager/Importer) porté ici (aucun reseau).
// ---------------------------------------------------------------------------

namespace {

const ImVec4 kOk = hex(0x9ece6a);      // en ligne / Partager
const ImVec4 kWarn = hex(0xe0af68);    // Importer
const ImVec4 kCardHover = hex(0x1e2229);

const char* const kHostedLoaders[] = {"Vanilla", "Fabric", "Forge", "NeoForge"};

// --- presse-papiers (C# Clipboard.GetText/SetText, Win32 CF_TEXT) ---------

std::string clip_get() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* p = static_cast<const char*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void clip_set(const std::string& s) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
    if (h) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, s.c_str(), s.size() + 1);
            GlobalUnlock(h);
        }
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

// --- helpers boutons colores (Partager vert, Importer orange) -------------

bool colored_button(const char* label, const ImVec2& size, const ImVec4& bg) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(bg.x + 0.10f, bg.y + 0.10f, bg.z + 0.10f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(bg.x - 0.12f, bg.y - 0.12f, bg.z - 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.06f, 0.08f, 1));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// Bouton desactive + tooltip (motif module deja employe ailleurs)
void disabled_btn(const char* label, const ImVec2& size) {
    ImGui::BeginDisabled();
    ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tr("Panneau serveur : portage à venir", "Server panel: port pending"));
}

// Wrap horizontal de cartes (remplace le WrapPanel C#)
struct CardFlow {
    float startX, x, y, rowH = 0, maxRight, spacing;
    CardFlow(float spacing = 8.0f)
        : startX(ImGui::GetCursorPosX()), x(startX),
          y(ImGui::GetCursorPosY()),
          maxRight(startX + ImGui::GetContentRegionAvail().x),
          spacing(spacing) {}
    void slot(float w, float /*h*/) {
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

void placeholder_card(const char* msg) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##empty", ImVec2(520, 110),
                      ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::SetCursorPos(ImVec2(32, 32));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 456.0f);
    ImGui::TextUnformatted(msg);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// --- Ping SLP (ServerPing.QueryAsync, TCP etat status protocole 1.7) ------
// Retour : ok = reponse valide ; errored = exception C# ("Impossible de
// joindre") ; sinon timeout 3,5 s = null C# ("Hors ligne").

void put_varint(std::string& out, int v) {
    for (;;) {
        unsigned char b = static_cast<unsigned char>(v & 0x7F);
        v >>= 7;
        if (v != 0) b |= 0x80;
        out.push_back(static_cast<char>(b));
        if (v == 0) return;
    }
}

bool read_varint_sock(SOCKET s, const std::atomic<bool>* cancel, int& out) {
    out = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (cancel && cancel->load()) return false;
        char b = 0;
        const int r = recv(s, &b, 1, 0);
        if (r != 1) return false;
        out |= (static_cast<unsigned char>(b) & 0x7F) << shift;
        if ((static_cast<unsigned char>(b) & 0x80) == 0) return true;
    }
    return false; // "Varint trop long."
}

bool read_exact(SOCKET s, char* buf, int n) {
    while (n > 0) {
        const int r = recv(s, buf, n, 0);
        if (r <= 0) return false;
        buf += r;
        n -= r;
    }
    return true;
}

// Decoupe "host:port" — port defaut 25565 (LastIndexOf(':') C#)
bool split_address(const std::string& addr, std::string& host, int& port) {
    port = 25565;
    const size_t c = addr.rfind(':');
    if (c == std::string::npos) {
        host = addr;
        return !host.empty();
    }
    const std::string ps = addr.substr(c + 1);
    if (ps.empty()) return false;
    int p = 0;
    for (char ch : ps) {
        if (ch < '0' || ch > '9') return false;
        p = p * 10 + (ch - '0');
        if (p > 65535) return false;
    }
    host = addr.substr(0, c);
    port = p;
    return !host.empty();
}

PingResult query_slp(const std::string& address,
                     const std::atomic<bool>* cancel) {
    PingResult r;
    r.started = true; // resultat final (timeout ou reponse), pas un placeholder
    static std::once_flag wsaOnce;
    std::call_once(wsaOnce, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });

    std::string host;
    int port = 25565;
    if (!split_address(address, host, port)) {
        r.errored = true;
        return r;
    }

    // Resolution (echec -> exception C# -> "Impossible de joindre")
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                    &res) != 0 ||
        !res) {
        r.errored = true;
        return r;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        r.errored = true;
        return r;
    }

    // Connect non bloquant, arbitre a 3,5 s par tranches de 200 ms
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    const int cr =
        connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    bool connected = cr == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        for (int waited = 0; waited < 3500 && !connected;
             waited += 200) {
            if (cancel && cancel->load()) {
                closesocket(s);
                return r; // abandon (shutdown) — resultat ignore
            }
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200 * 1000;
            const int sel = select(0, nullptr, &wf, nullptr, &tv);
            if (sel > 0) {
                int err = 0;
                int len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&err), &len);
                connected = err == 0;
                break;
            }
            if (sel < 0) break;
        }
        if (!connected) {
            closesocket(s);
            return r; // timeout -> null C# -> "Hors ligne"
        }
    } else if (!connected) {
        closesocket(s);
        r.errored = true;
        return r;
    }

    // Bloquant + timeout recv 3,5 s (C# : ReadTimeout inappliqué aux async,
    // on applique ici — plus sûr, divergence sans effet UI visible)
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 3500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&to),
               sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&to),
               sizeof(to));

    // Handshake (VarInt 0x00, proto 47, host, port BE, next state 1)
    std::string hs;
    put_varint(hs, 0x00);
    put_varint(hs, 47);
    put_varint(hs, static_cast<int>(host.size()));
    hs += host;
    hs.push_back(static_cast<char>((port >> 8) & 0xFF));
    hs.push_back(static_cast<char>(port & 0xFF));
    put_varint(hs, 1);
    std::string frame;
    put_varint(frame, static_cast<int>(hs.size()));
    frame += hs;
    const char req[2] = {0x01, 0x00}; // status request
    if (send(s, frame.data(), static_cast<int>(frame.size()), 0) !=
            static_cast<int>(frame.size()) ||
        send(s, req, 2, 0) != 2) {
        closesocket(s);
        r.errored = true;
        return r;
    }

    // Reponse : len, packet id (0x00), json len, json
    int plen = 0, pid = 0, jlen = 0;
    if (!read_varint_sock(s, cancel, plen) || !read_varint_sock(s, cancel, pid) ||
        pid != 0 || !read_varint_sock(s, cancel, jlen) || jlen <= 0 ||
        jlen > 4 * 1024 * 1024) {
        closesocket(s);
        r.errored = true;
        return r;
    }
    std::string js(static_cast<size_t>(jlen), '\0');
    const bool got = read_exact(s, js.data(), jlen);
    closesocket(s);
    if (!got) {
        r.errored = true;
        return r;
    }

    // Parsing JSON (players.online/max, version.name, description)
    try {
        const nlohmann::json j = nlohmann::json::parse(js);
        if (j.contains("players") && j["players"].is_object()) {
            r.online = j["players"].value("online", 0);
            r.max = j["players"].value("max", 0);
        }
        if (j.contains("version") && j["version"].is_object())
            r.version = j["version"].value("name", "");
        std::string motd;
        if (j.contains("description")) {
            const auto& d = j["description"];
            if (d.is_string())
                motd = d.get<std::string>();
            else if (d.is_object())
                motd = d.value("text", "");
        }
        std::replace(motd.begin(), motd.end(), '\n', ' ');
        r.motd = motd;
        r.ok = true;
    } catch (...) {
        r.errored = true;
    }
    return r;
}

void ping_worker() {
    for (;;) {
        std::string addr;
        {
            std::unique_lock lk(serversState.m);
            serversState.cv.wait(lk, [] {
                return serversState.workPending || serversState.cancel;
            });
            if (serversState.cancel) return;
            if (serversState.targets.empty()) {
                serversState.workPending = false;
                serversState.pinging = false;
                continue;
            }
            addr = serversState.targets.front();
            serversState.targets.erase(serversState.targets.begin());
        }
        PingResult r = query_slp(addr, &serversState.cancel);
        {
            std::lock_guard lk(serversState.m);
            if (!serversState.cancel) serversState.pings[addr] = r;
        }
    }
}

// Depose les adresses a pinger (re-jeu comme RenderFavoriteCards C#).
// Un placeholder (started=false) pose dans pings pour eviter le re-jeu
// en boucle pendant que le worker est en train de pinger l'adresse.
void queue_pings(const std::vector<std::string>& addrs) {
    std::lock_guard lk(serversState.m);
    if (!serversState.workerStarted) {
        serversState.workerStarted = true;
        serversState.th = std::thread(ping_worker);
    }
    for (const auto& a : addrs) {
        if (serversState.pings.count(a)) continue; // placeholder ou resultat
        serversState.pings[a] = PingResult{};
        serversState.targets.push_back(a);
        serversState.pinging = true;
    }
    if (!serversState.targets.empty()) {
        serversState.workPending = true;
        serversState.cv.notify_one();
    }
}

// --- états locaux (buffers persistants) -----------------------------------

char s_favBuf[128] = "";
char s_cityName[64] = "";
char s_cityOwner[64] = "";
char s_cityAddr[128] = "";
char s_cityDesc[256] = "";

// modale creation serveur herbe
bool s_nsRequest = false;
bool s_nsWasOpen = false;
char s_nsName[64] = "";
char s_nsVer[64] = "";
int s_nsLoader = 0;
int s_nsPort = 25565;
int s_nsRam = 2;

// modale suppression (par Id)
bool s_delRequest = false;
bool s_delWasOpen = false;
std::string s_delId;

std::string iso_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// --- Onglet 1 : Mes serveurs ----------------------------------------------

void hosted_card(const nlohmann::json& s, int idx) {
    const float w = 340.0f, h = 140.0f;
    ImGui::PushID(idx);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##hosted", ImVec2(w, h), ImGuiChildFlags_Borders);

    const std::string name = s.value("Name", "?");
    // pastille statut (ServerHost absent -> "Non installé", gris)
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + 5.0f, p.y + 9.0f), 5.0f,
        ImGui::ColorConvertFloat4ToU32(kDim));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18.0f);
    ImGui::TextUnformatted(name.c_str());

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (fSmall) ImGui::PushFont(fSmall);
    ImGui::TextUnformatted(tr("Non installé"));
    ImGui::Text("%s %s  ·  port %d  ·  RAM %d Go",
                s.value("Loader", "Vanilla").c_str(),
                s.value("McVersion", "").c_str(), s.value("Port", 25565),
                s.value("MaxRamGb", 2));
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    disabled_btn(tr("Gérer"), ImVec2(90, 30));
    ImGui::SameLine();
    disabled_btn(tr("Démarrer"), ImVec2(100, 30));
    ImGui::SameLine();
    if (ImGui::Button(tr("Supprimer"), ImVec2(100, 30))) {
        s_delId = s.value("Id", "");
        s_delRequest = true;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void tab_hosted() {
    if (accent_button(tr("Nouveau serveur"), ImVec2(170, 34))) s_nsRequest = true;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (fSmall) ImGui::PushFont(fSmall);
    ImGui::TextWrapped(
        "%s", tr("Clique sur un serveur pour ouvrir le panneau de gestion "
                 "(console, joueurs, map, réglages)."));
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    auto& arr = DataStore::settings.hostedServers;
    if (!arr.is_array() || arr.empty()) {
        placeholder_card(
            tr("Aucun serveur hébergé.\n"
               "Clique sur « Nouveau serveur » pour en créer un."));
        return;
    }
    CardFlow flow(8.0f);
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_object()) continue;
        flow.slot(340.0f, 140.0f);
        hosted_card(arr[i], static_cast<int>(i));
        flow.advance(340.0f, 140.0f);
    }
    flow.end();
}

// --- Onglet 2 : Favoris ----------------------------------------------------

void favorite_card(const std::string& addr, int idx) {
    const float w = 250.0f, h = 132.0f;
    ImGui::PushID(idx);
    PingResult pr;
    bool has = false;
    {
        std::lock_guard lk(serversState.m);
        auto it = serversState.pings.find(addr);
        if (it != serversState.pings.end()) {
            pr = it->second;
            has = true;
        }
    }
    const bool hov = ImGui::IsMouseHoveringRect(
        ImGui::GetCursorScreenPos(),
        ImVec2(ImGui::GetCursorScreenPos().x + w,
               ImGui::GetCursorScreenPos().y + h));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hov ? kCardHover : kCard);
    ImGui::BeginChild("##fav", ImVec2(w, h), ImGuiChildFlags_Borders);

    // titre + sous-titre (adresse dupliquee, fidele C#)
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 220.0f);
    ImGui::TextUnformatted(addr.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (fSmall) ImGui::PushFont(fSmall);
    ImGui::TextUnformatted(addr.c_str());

    // statut colore (started = le ping a abouti : reponse ou timeout)
    ImGui::Spacing();
    if (!has || !pr.started)
        ImGui::TextUnformatted(tr("Ping en cours..."));
    else if (pr.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, kOk);
        ImGui::TextUnformatted(tr("En ligne"));
        ImGui::PopStyleColor();
    } else if (pr.errored) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(tr("Impossible de joindre"));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(tr("Hors ligne"));
        ImGui::PopStyleColor();
    }
    // info joueurs/version (accent)
    if (has && pr.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        if (!pr.motd.empty())
            ImGui::Text("%d/%d joueurs · %s\n%s", pr.online, pr.max,
                        pr.version.c_str(), pr.motd.c_str());
        else
            ImGui::Text("%d/%d joueurs · %s", pr.online, pr.max,
                        pr.version.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    // Double-clic -> copie l'adresse (C# DoubleTapped)
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        clip_set(addr);
    ImGui::PopID();
}

void tab_favorites() {
    // Champ + actions (Enter -> Ajouter)
    ImGui::SetNextItemWidth(300.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##favaddr", "Adresse du serveur (ex: mc.example.com)", s_favBuf,
        sizeof(s_favBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    auto doAdd = [&] {
        const std::string a = trimmed(s_favBuf);
        if (a.empty()) return;
        auto& favs = DataStore::settings.favoriteServers;
        for (const auto& f : favs)
            if (iequals(f, a)) {
                s_favBuf[0] = '\0';
                return; // doublon silencieux
            }
        favs.push_back(a);
        DataStore::save();
        s_favBuf[0] = '\0';
        queue_pings(favs);
    };
    if (enter) doAdd();
    if (accent_button(tr("+ Ajouter"), ImVec2(100, 0))) doAdd();
    ImGui::SameLine();
    if (danger_button(tr("- Supprimer"), ImVec2(110, 0))) {
        // Fidele C# : lit le CHAMP de saisie, pas une selection de carte
        const std::string a = trimmed(s_favBuf);
        if (!a.empty()) {
            auto& favs = DataStore::settings.favoriteServers;
            for (auto it = favs.begin(); it != favs.end(); ++it)
                if (iequals(*it, a)) {
                    favs.erase(it);
                    break;
                }
            DataStore::save();
            s_favBuf[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Actualiser"), ImVec2(110, 0))) {
        std::lock_guard lk(serversState.m);
        serversState.pings.clear(); // re-jeu de tous les pings
    }
    ImGui::Spacing();

    auto& favs = DataStore::settings.favoriteServers;
    if (favs.empty()) {
        placeholder_card("Aucun serveur favori.\nAjoutez une adresse ci-dessus.");
        return;
    }
    // Nouvelle liste -> lance les pings (comme RenderFavoriteCards)
    queue_pings(favs);

    CardFlow flow(8.0f);
    for (size_t i = 0; i < favs.size(); ++i) {
        flow.slot(250.0f, 132.0f);
        favorite_card(favs[i], static_cast<int>(i));
        flow.advance(250.0f, 132.0f);
    }
    flow.end();
}

// --- Onglet 3 : Villes de la team -----------------------------------------

void city_card(const nlohmann::json& c, int idx) {
    const float w = 270.0f, h = 168.0f;
    ImGui::PushID(idx);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##city", ImVec2(w, h), ImGuiChildFlags_Borders);

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted(c.value("Name", "").c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (fSmall) ImGui::PushFont(fSmall);
    ImGui::Text(tr("Propriétaire : %s", "Owner: %s"), c.value("Owner", "").c_str());
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::TextWrapped("%s", c.value("Address", "").c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (fSmall) ImGui::PushFont(fSmall);
    ImGui::TextWrapped("%s", c.value("Description", "").c_str());
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    if (accent_button(tr("Modifier"), ImVec2(96, 26))) {
        // Fidele C# (quirk destructeur) : repli dans le formulaire + retrait
        const std::string id = c.value("Id", "");
        std::snprintf(s_cityName, sizeof(s_cityName), "%s",
                      c.value("Name", "").c_str());
        std::snprintf(s_cityOwner, sizeof(s_cityOwner), "%s",
                      c.value("Owner", "").c_str());
        std::snprintf(s_cityAddr, sizeof(s_cityAddr), "%s",
                      c.value("Address", "").c_str());
        std::snprintf(s_cityDesc, sizeof(s_cityDesc), "%s",
                      c.value("Description", "").c_str());
        auto& arr = DataStore::settings.cities;
        for (auto it = arr.begin(); it != arr.end(); ++it)
            if (it->is_object() && it->value("Id", "") == id) {
                arr.erase(it);
                break;
            }
        DataStore::save();
    }
    ImGui::SameLine();
    if (danger_button(tr("Supprimer"), ImVec2(100, 26))) {
        const std::string id = c.value("Id", "");
        auto& arr = DataStore::settings.cities;
        for (auto it = arr.begin(); it != arr.end(); ++it)
            if (it->is_object() && it->value("Id", "") == id) {
                arr.erase(it);
                break;
            }
        DataStore::save();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void tab_cities() {
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##cityname", "Nom de la ville", s_cityName,
                             sizeof(s_cityName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##cityowner", "Propriétaire", s_cityOwner,
                             sizeof(s_cityOwner));
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##cityaddr", "Adresse du serveur", s_cityAddr,
                             sizeof(s_cityAddr));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(250.0f);
    ImGui::InputTextWithHint("##citydesc", "Description", s_cityDesc,
                             sizeof(s_cityDesc));
    ImGui::Spacing();

    if (accent_button(tr("+ Ajouter"), ImVec2(110, 0))) {
        if (trimmed(s_cityName)[0] != '\0' &&
            trimmed(s_cityAddr)[0] != '\0') {
            auto& arr = DataStore::settings.cities;
            if (!arr.is_array()) arr = nlohmann::json::array();
            arr.push_back({{"Id", new_guid()},
                           {"Name", trimmed(s_cityName)},
                           {"Owner", trimmed(s_cityOwner)},
                           {"Address", trimmed(s_cityAddr)},
                           {"Description", trimmed(s_cityDesc)}});
            DataStore::save();
            s_cityName[0] = s_cityOwner[0] = s_cityAddr[0] =
                s_cityDesc[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (colored_button(tr("Partager"), ImVec2(110, 0), kOk)) {
        // Copie l'adresse de la ville dont le nom correspond au champ
        const std::string n = trimmed(s_cityName);
        if (!n.empty())
            for (const auto& c : DataStore::settings.cities)
                if (c.is_object() && iequals(c.value("Name", ""), n)) {
                    clip_set(c.value("Address", ""));
                    break;
                }
    }
    ImGui::SameLine();
    if (colored_button(tr("Importer"), ImVec2(110, 0), kWarn)) {
        const std::string clip = trimmed(clip_get().c_str());
        if (!clip.empty())
            std::snprintf(s_cityAddr, sizeof(s_cityAddr), "%s",
                          clip.c_str());
    }
    ImGui::Spacing();

    auto& arr = DataStore::settings.cities;
    if (!arr.is_array() || arr.empty()) {
        placeholder_card("Aucune ville.\nAjoutez-en une ci-dessus.");
        return;
    }
    CardFlow flow(8.0f);
    int shown = 0;
    for (const auto& c : arr) {
        if (!c.is_object()) continue;
        flow.slot(270.0f, 168.0f);
        city_card(c, shown);
        flow.advance(270.0f, 168.0f);
        ++shown;
    }
    flow.end();
}

// --- Modales (creation / suppression) -------------------------------------

void server_modals() {
    const ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f,
                        ImGui::GetIO().DisplaySize.y * 0.5f);
    auto label = [](const char* t) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
    };

    // Creation : ouverture
    if (s_nsRequest) {
        ImGui::OpenPopup("###newhosted");
        s_nsRequest = false;
        s_nsWasOpen = false;
    }
    if (s_delRequest) {
        ImGui::OpenPopup("###delsrv");
        s_delRequest = false;
        s_delWasOpen = false;
    }

    // Creation : formulaire (creation AU moment de la fermeture, si nom non
    // vide — fidele ShowDialog C# qui lit les champs apres fermeture)
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 430), ImGuiCond_Appearing);
    bool open = true;
    const std::string nsTitle = std::string(tr("Nouveau serveur hébergé")) + "###newhosted";
    if (ImGui::BeginPopupModal(nsTitle.c_str(), &open,
                               ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize)) {
        s_nsWasOpen = true;
        label(tr("Nom :"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##nsname", "Nom du serveur", s_nsName,
                                 sizeof(s_nsName));
        label(tr("Version Minecraft :"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##nsver", "Version Minecraft (ex: 1.21.1)",
                                 s_nsVer, sizeof(s_nsVer));
        label(tr("Chargeur :"));
        ImGui::SetNextItemWidth(200);
        ImGui::Combo("##nsloader", &s_nsLoader, kHostedLoaders, 4);
        label(tr("Port :"));
        ImGui::SetNextItemWidth(160);
        ImGui::InputInt("##nsport", &s_nsPort);
        if (s_nsPort < 1024) s_nsPort = 1024;
        if (s_nsPort > 65535) s_nsPort = 65535;
        label(tr("RAM max (Go) :"));
        ImGui::SetNextItemWidth(160);
        ImGui::InputInt("##nsram", &s_nsRam);
        if (s_nsRam < 1) s_nsRam = 1;
        if (s_nsRam > 16) s_nsRam = 16;
        ImGui::Spacing();
        if (accent_button(tr("Créer"), ImVec2(130, 36)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else if (s_nsWasOpen) {
        s_nsWasOpen = false;
        // Fermeture (Croix ou Créer) : nom vide = annulation silencieuse
        const std::string name = trimmed(s_nsName);
        if (!name.empty()) {
            auto& arr = DataStore::settings.hostedServers;
            if (!arr.is_array()) arr = nlohmann::json::array();
            arr.push_back(
                {{"Id", new_guid()},
                 {"Name", name},
                 {"McVersion", trimmed(s_nsVer)},
                 {"Loader", kHostedLoaders[s_nsLoader]},
                 {"Port", s_nsPort},
                 {"Motd", "Serveur hébergé par Team Launcher"},
                 {"MaxRamGb", s_nsRam},
                 {"JavaMajor", 8},
                 {"AutoRestart", true},
                 {"RestartAt", ""},
                 {"CreatedAt", iso_now()},
                 {"PublicAddress", ""},
                 {"RpProfile", false},
                 {"WhitelistEnabled", false},
                 {"Whitelist", nlohmann::json::array()},
                 {"WelcomeMessage", ""},
                 {"DiscordWebhookUrl", ""}});
            DataStore::save();
        }
        s_nsName[0] = s_nsVer[0] = '\0';
        s_nsLoader = 0;
        s_nsPort = 25565;
        s_nsRam = 2;
    }

    // Suppression
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360, 150), ImGuiCond_Appearing);
    bool open2 = true;
    const std::string delTitle = std::string(tr("Supprimer")) + "###delsrv";
    if (ImGui::BeginPopupModal(delTitle.c_str(), &open2,
                               ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize)) {
        s_delWasOpen = true;
        ImGui::TextUnformatted(tr("Supprimer ce serveur ?"));
        ImGui::Spacing();
        if (ImGui::Button(tr("Annuler"), ImVec2(120, 36)))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (danger_button(tr("Supprimer"), ImVec2(120, 36))) {
            auto& arr = DataStore::settings.hostedServers;
            for (auto it = arr.begin(); it != arr.end(); ++it)
                if (it->is_object() && it->value("Id", "") == s_delId) {
                    arr.erase(it);
                    break;
                }
            DataStore::save();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (s_delWasOpen) {
        s_delWasOpen = false;
    }
}

} // namespace

void servers_page() {
    // TL_AUTO_TAB (test) : ouvre directement l'onglet 0..2 (une seule fois)
    static int autoTab = -1;
    static bool autoTabInit = false;
    if (!autoTabInit) {
        autoTabInit = true;
        if (const char* t = std::getenv("TL_AUTO_TAB")) {
            const int i = std::atoi(t);
            if (i >= 0 && i <= 2) autoTab = i;
        }
    }

    ImGui::BeginChild("##srvscroll", ImVec2(0, 0));

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Serveurs"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##srvtabs")) {
        ImGui::PushStyleColor(ImGuiCol_Tab, kCard);
        ImGui::PushStyleColor(ImGuiCol_TabHovered, kCardHover);
        ImGui::PushStyleColor(ImGuiCol_TabActive, kAccent);
        const int forced = (autoTab >= 0) ? autoTab : -1;
        if (ImGui::BeginTabItem(tr("Mes serveurs"), nullptr,
                                forced == 0 ? ImGuiTabItemFlags_SetSelected
                                            : 0)) {
            if (forced == 0) autoTab = -1; // une seule fois
            tab_hosted();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Favoris"), nullptr,
                                forced == 1 ? ImGuiTabItemFlags_SetSelected
                                            : 0)) {
            if (forced == 1) autoTab = -1;
            tab_favorites();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Villes de la team"), nullptr,
                                forced == 2 ? ImGuiTabItemFlags_SetSelected
                                            : 0)) {
            if (forced == 2) autoTab = -1;
            tab_cities();
            ImGui::EndTabItem();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndTabBar();
    }

    server_modals();
    ImGui::EndChild();
}

} // namespace tl::ui
