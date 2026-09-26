#include "skin_service.hpp"

#include "datastore.hpp"
#include "http_win.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace tl::skin {
namespace {

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

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// SkinService.InstallCustomSkinLoaderAsync (Modrinth, Forge).
fs::path install_customskinloader(const nlohmann::json& inst,
                                  const fs::path& modsDir,
                                  const std::atomic<bool>* cancel) {
    std::string mcVersion = inst.value("McVersion", "latest");
    if (mcVersion == "latest" || mcVersion == "?" || mcVersion.empty())
        mcVersion = latest_release();

    const std::string url =
        "https://api.modrinth.com/v2/project/customskinloader/version"
        "?loaders=%5B%22forge%22%5D&game_versions=%5B%22" +
        url_esc(mcVersion) + "%22%5D";
    const auto resp = http::get_string(url, cancel);
    if (!resp) throw std::runtime_error("Modrinth inaccessible.");
    const auto doc = nlohmann::json::parse(*resp, nullptr, false);
    if (!doc.is_array() || doc.empty())
        throw std::runtime_error("CustomSkinLoader introuvable pour Forge " +
                                 mcVersion + ".");

    const nlohmann::json* file = nullptr;
    const auto& files = doc[0].value("files", nlohmann::json::array());
    if (files.is_array() && !files.empty()) {
        for (const auto& f : files)
            if (f.value("primary", false)) {
                file = &f;
                break;
            }
        if (!file) file = &files[0];
    }
    if (!file)
        throw std::runtime_error("CustomSkinLoader introuvable pour Forge " +
                                 mcVersion + ".");

    const std::string fileName = file->value("filename", "customskinloader.jar");
    const std::string fileUrl = file->value("url", "");
    const fs::path dest = modsDir / fileName;
    if (fileUrl.empty() || !http::get_to_file(fileUrl, dest, nullptr, cancel))
        throw std::runtime_error("Telechargement de CustomSkinLoader echoue.");
    return dest;
}

} // namespace

std::string latest_release() {
    static std::string cache;
    if (!cache.empty()) return cache;
    const auto j = http::get_string(
        "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
    if (!j) throw std::runtime_error("Manifeste de versions Mojang inaccessible.");
    const auto doc = nlohmann::json::parse(*j, nullptr, false);
    if (doc.is_object()) {
        const auto& versions = doc.value("versions", nlohmann::json::array());
        for (const auto& v : versions)
            if (v.value("type", "") == "release") {
                cache = v.value("id", "");
                break;
            }
    }
    if (cache.empty())
        throw std::runtime_error("Aucune version release trouvee.");
    return cache;
}

std::string apply(const nlohmann::json& inst, const std::string& skinPath,
                  const std::string& playerName,
                  const std::atomic<bool>* cancel) {
    const fs::path root = DataStore::instancesRoot() / inst.value("Id", "");
    const fs::path modsDir = root / "mods";
    std::error_code ec;
    fs::create_directories(modsDir, ec);

    // 1. CustomSkinLoader deja pose ?
    fs::path csl;
    if (fs::exists(modsDir, ec))
        for (const auto& e : fs::directory_iterator(modsDir, ec))
            if (e.is_regular_file() &&
                lower_copy(e.path().filename().string())
                        .find("customskinloader") != std::string::npos) {
                csl = e.path();
                break;
            }
    if (csl.empty()) csl = install_customskinloader(inst, modsDir, cancel);

    // 2. Copie du skin sous le pseudo (source LocalSkin de CustomSkinLoader)
    const fs::path localDir = root / "config" / "CustomSkinLoader" / "LocalSkin";
    fs::create_directories(localDir, ec);
    fs::copy_file(skinPath, localDir / (playerName + ".png"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("Copie du skin echouee : " + ec.message());
    return csl.filename().string();
}

bool download_by_name(const std::string& name, const fs::path& dest,
                      const std::atomic<bool>* cancel) {
    return http::get_to_file("https://mc-heads.net/skin/" + url_esc(name),
                             dest, nullptr, cancel);
}

} // namespace tl::skin
