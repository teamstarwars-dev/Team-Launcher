#include "diagapi.hpp"

#include "crash_analyzer.hpp"
#include "modcheck.hpp"
#include "modmeta.hpp"

#include <nlohmann/json.hpp>

namespace tl::diagapi {

namespace {

Result error(int status, const std::string& message) {
    return Result{status, nlohmann::json{{"error", message}}.dump()};
}

} // namespace

Result crash(const std::string& body) {
    std::string logText = body;
    // Un log peut parfaitement commencer par une accolade ; on n'essaie
    // donc le JSON que par opportunisme, et l'échec n'est pas une erreur.
    if (!logText.empty() && logText.front() == '{') {
        try {
            const auto j = nlohmann::json::parse(logText);
            if (j.is_object() && j.contains("log") && j["log"].is_string())
                logText = j["log"].get<std::string>();
        } catch (const std::exception&) {
            // C'était un log, pas du JSON. On le garde tel quel.
        }
    }

    const auto r = crash::analyze_report(logText);
    nlohmann::json out{
        {"found", r.found},
        {"cause", crash::cause_key(r.cause)},
        {"title", r.title},
        {"action", r.action},
        {"loader", r.loader},
        {"mcVersion", r.mcVersion},
        {"exception", r.exception},
        {"summary", r.summary},
    };
    out["suspects"] = nlohmann::json::array();
    for (const auto& s : r.suspects)
        out["suspects"].push_back({{"modId", s.modId},
                                   {"version", s.version},
                                   {"source", s.source},
                                   {"evidence", s.evidence},
                                   {"score", s.score}});
    return Result{200, out.dump()};
}

Result mods(const std::string& body) {
    nlohmann::json in;
    try {
        in = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        return error(400, std::string("JSON illisible : ") + ex.what());
    }
    if (!in.is_object() || !in.contains("mods") || !in["mods"].is_array())
        return error(400, "attendu : {loader, mcVersion, mods:[...]}");

    const std::string loader = in.value("loader", std::string{});
    const std::string mc = in.value("mcVersion", std::string{});
    // `loader` absent donnerait « aucun chargeur sur cette instance » —
    // diagnostic juste pour le launcher, trompeur pour un appelant qui a
    // simplement oublié le champ. On le lui dit.
    if (loader.empty())
        return error(400,
                     "champ « loader » requis (Vanilla, Fabric, Forge, "
                     "NeoForge ou Quilt)");

    std::vector<modmeta::Mod> list;
    for (const auto& e : in["mods"]) {
        if (!e.is_object()) continue;
        modmeta::Mod m;
        // Deux formes acceptées : un manifeste brut à analyser, ou un
        // descripteur déjà analysé. La première évite à l'appelant de
        // réimplémenter la lecture des cinq formats — c'est justement ce
        // qu'il vient chercher.
        const std::string manifest = e.value("manifest", std::string{});
        if (!manifest.empty()) {
            const std::string kind = e.value("format", std::string("auto"));
            if (kind == "mods.toml" || kind == "forge")
                m = modmeta::parse_mods_toml(manifest, false);
            else if (kind == "neoforge.mods.toml" || kind == "neoforge")
                m = modmeta::parse_mods_toml(manifest, true);
            else if (kind == "quilt")
                m = modmeta::parse_quilt(manifest);
            else if (kind == "mcmod.info")
                m = modmeta::parse_mcmod_info(manifest);
            else if (manifest.find("[[mods]]") != std::string::npos)
                m = modmeta::parse_mods_toml(
                    manifest, manifest.find("neoforge") != std::string::npos);
            else if (manifest.find("quilt_loader") != std::string::npos)
                m = modmeta::parse_quilt(manifest);
            else
                m = modmeta::parse_fabric(manifest);
        } else {
            m.id = e.value("id", std::string{});
            m.name = e.value("name", m.id);
            m.version = e.value("version", std::string{});
            m.mcRange = e.value("mcRange", std::string{});
            m.loader = modmeta::loader_from_string(
                e.value("loader", std::string{}));
            if (auto d = e.find("deps"); d != e.end() && d->is_array())
                for (const auto& x : *d) {
                    if (!x.is_object()) continue;
                    modmeta::Dep dep;
                    dep.id = x.value("id", std::string{});
                    dep.mandatory = x.value("mandatory", true);
                    dep.range = x.value("range", std::string{});
                    if (!dep.id.empty()) m.deps.push_back(std::move(dep));
                }
        }
        m.file = e.value("file", m.file.empty() ? m.id : m.file);
        m.disabled = e.value("disabled", false);
        if (m.name.empty()) m.name = m.id.empty() ? m.file : m.id;
        list.push_back(std::move(m));
    }

    const auto rep = modcheck::analyse(list, loader, mc);
    nlohmann::json out{
        {"errors", rep.errors},   {"warnings", rep.warnings},
        {"infos", rep.infos},     {"analysed", rep.analysed},
        {"skipped", rep.skipped}, {"suggestions", rep.suggestions},
    };
    out["issues"] = nlohmann::json::array();
    for (const auto& i : rep.issues)
        // `severity` est une clé STABLE, en anglais : c'est un contrat
        // d'API, et un appelant ne doit pas avoir à comparer des chaînes
        // françaises susceptibles d'être retraduites. Le libellé lisible
        // part à côté, sous un autre nom.
        out["issues"].push_back(
            {{"severity", i.sev == modcheck::Severity::Error     ? "error"
                          : i.sev == modcheck::Severity::Warning ? "warning"
                                                                 : "info"},
             {"severityLabel", modcheck::severity_label(i.sev)},
             {"file", i.file},
             {"title", i.title},
             {"detail", i.detail}});
    return Result{200, out.dump()};
}

std::string describe() {
    return R"({"name":"Team Launcher API","version":"1","endpoints":[)"
           R"({"path":"/v1/status","method":"GET","scope":"read"},)"
           R"({"path":"/v1/instances","method":"GET","scope":"read"},)"
           R"({"path":"/v1/launch","method":"POST","scope":"control"},)"
           R"({"path":"/v1/diag/crash","method":"POST","scope":"diag"},)"
           R"({"path":"/v1/diag/mods","method":"POST","scope":"diag"}]})";
}

} // namespace tl::diagapi
