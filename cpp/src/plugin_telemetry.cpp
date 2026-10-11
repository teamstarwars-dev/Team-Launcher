#include "plugin_telemetry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>

namespace tl::plugtele {
namespace {

constexpr std::size_t kMaxKeys = 200;  // = plafond du serveur par envoi
constexpr long kMaxCount = 10000;      // idem

std::mutex g_m;
Counts g_counts;

void add_locked(const Counts::key_type& k, long n) {
    auto it = g_counts.find(k);
    if (it == g_counts.end()) {
        if (g_counts.size() >= kMaxKeys) return;
        it = g_counts.emplace(k, 0).first;
    }
    it->second = std::min(kMaxCount, it->second + n);
}

} // namespace

std::string classify(const std::string& kind, const std::string& detail) {
    if (kind == "failed") return "start_failed";
    if (kind == "exited") return "exited";
    if (kind == "denied") return "denied";
    if (kind == "killed") {
        // Mêmes textes que plugin_host_main.cpp (kill_plugin).
        if (detail == "débit abusif") return "rate_abuse";
        if (detail == "permissions sondées à répétition") return "permission_probe";
        if (detail == "requêtes invalides répétées") return "invalid_requests";
        if (detail == "trame invalide") return "bad_frame";
        return "killed_other";
    }
    return {};
}

void record(const std::string& plugin, const std::string& version, const std::string& kind) {
    if (plugin.empty() || version.empty() || kind.empty()) return;
    std::lock_guard<std::mutex> lk(g_m);
    add_locked({plugin, version, kind}, 1);
}

Counts take() {
    std::lock_guard<std::mutex> lk(g_m);
    Counts out;
    out.swap(g_counts);
    return out;
}

void restore(const Counts& c) {
    std::lock_guard<std::mutex> lk(g_m);
    for (const auto& [k, n] : c) add_locked(k, n);
}

std::size_t pending() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_counts.size();
}

std::string payload(const Counts& c, const std::string& launcherVersion, const std::string& os) {
    nlohmann::json ev = nlohmann::json::array();
    for (const auto& [k, n] : c)
        ev.push_back({{"plugin", std::get<0>(k)},
                      {"version", std::get<1>(k)},
                      {"kind", std::get<2>(k)},
                      {"count", n}});
    return nlohmann::json{{"v", 1}, {"launcher", launcherVersion}, {"os", os}, {"events", ev}}
        .dump();
}

void reset_for_tests() {
    std::lock_guard<std::mutex> lk(g_m);
    g_counts.clear();
}

} // namespace tl::plugtele
