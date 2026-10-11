// Interface de review. Tout ce qui vient du serveur — noms, manifestes,
// diff des sources — a été écrit par des développeurs qu'on est justement
// en train de relire : on ne l'insère JAMAIS comme HTML, uniquement en
// texte (textContent). La CSP du serveur interdit de toute façon les
// scripts en ligne.
"use strict";

const $ = (s) => document.querySelector(s);
const RISKY = new Set(["game:launch"]);
const PERM_TEXT = {
  "instances:read": "lire la liste des instances",
  "mods:read": "lire la liste des mods",
  "ui:notify": "afficher des notifications",
  "game:launch": "LANCER LE JEU",
};

let token = null;
try { token = sessionStorage.getItem("tl-review-token"); } catch { /* stockage bloqué */ }
let me = null;
let current = null; // soumission affichée

function saveToken(t) {
  token = t;
  try { t ? sessionStorage.setItem("tl-review-token", t) : sessionStorage.removeItem("tl-review-token"); } catch { }
}

function status(msg, err = false) {
  const s = $("#status");
  s.textContent = msg || "";
  s.classList.toggle("err", err);
}

async function api(method, path, body) {
  const headers = {};
  if (token) headers.Authorization = "Bearer " + token;
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const r = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  const text = await r.text();
  let data = text;
  try { data = JSON.parse(text); } catch { }
  if (!r.ok) {
    const d = data && data.detail;
    const msg = typeof d === "string" ? d : `erreur ${r.status}`;
    if (r.status === 401) { saveToken(null); show("login"); }
    throw new Error(msg);
  }
  return data;
}

function show(which) {
  for (const id of ["login", "register", "mfa", "app"]) $("#" + id).hidden = id !== which;
  $("#logout").hidden = which === "login" || which === "register";
}

function el(tag, text, cls) {
  const e = document.createElement(tag);
  if (text !== undefined) e.textContent = String(text);
  if (cls) e.className = cls;
  return e;
}

function date(ts) {
  return new Date(ts * 1000).toLocaleString("fr-FR", { dateStyle: "short", timeStyle: "short" });
}

// --- Authentification --------------------------------------------------------

async function start() {
  if (!token) return show("login");
  try {
    me = await api("GET", "/v1/me");
  } catch { return; }
  $("#who").textContent = me.displayName;
  if (!me.totpEnabled) return setupMfa();
  if (!me.mfaOk) { $("#mfa-setup").hidden = true; return show("mfa"); }
  if (!me.reviewer && !me.admin) {
    show("app");
    $("#queue-view").hidden = true;
    return status("Ce compte n'a pas le rôle de relecteur.", true);
  }
  $("#tab-admin").hidden = !me.admin;
  show("app");
  loadQueue();
}

async function setupMfa() {
  const r = await api("POST", "/v1/auth/totp/setup");
  $("#mfa-secret").textContent = r.secret;
  $("#mfa-uri").textContent = r.uri;
  $("#mfa-setup").hidden = false;
  show("mfa");
}

$("#login-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  try {
    const r = await api("POST", "/v1/auth/login", { email: f.get("email"), password: f.get("password") });
    saveToken(r.token);
    ev.target.reset();
    status("");
    start();
  } catch (e) { status(e.message, true); }
});

$("#to-register").addEventListener("click", () => { status(""); show("register"); });
$("#to-login").addEventListener("click", () => { status(""); show("login"); });

$("#register-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  const email = String(f.get("email")).trim();
  const password = String(f.get("password"));
  if (password !== String(f.get("password2"))) return status("Les deux mots de passe diffèrent.", true);
  try {
    await api("POST", "/v1/auth/register",
              { email, password, display_name: String(f.get("display_name")).trim() });
    // Connexion immédiate, puis mise en place de la double authentification.
    const r = await api("POST", "/v1/auth/login", { email, password });
    saveToken(r.token);
    ev.target.reset();
    status("Compte créé. Configurez maintenant la double authentification.");
    start();
  } catch (e) { status(e.message, true); }
});

$("#mfa-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const code = new FormData(ev.target).get("code");
  const path = $("#mfa-setup").hidden ? "/v1/auth/totp/verify" : "/v1/auth/totp/confirm";
  try {
    await api("POST", path, { code });
    ev.target.reset();
    $("#mfa-setup").hidden = true;
    $("#mfa-secret").textContent = $("#mfa-uri").textContent = "";
    status("");
    start();
  } catch (e) { status(e.message, true); }
});

$("#logout").addEventListener("click", async () => {
  try { await api("POST", "/v1/auth/logout"); } catch { }
  saveToken(null);
  location.reload();
});

// --- File d'attente -------------------------------------------------------------

async function loadQueue() {
  $("#detail").hidden = true;
  $("#queue-view").hidden = false;
  let rows;
  try { rows = await api("GET", "/v1/review/queue"); } catch (e) { return status(e.message, true); }
  const tb = $("#queue tbody");
  tb.replaceChildren();
  for (const r of rows) {
    const tr = el("tr");
    const name = el("td");
    name.append(el("strong", r.name), " ", el("span", r.slug, "muted"));
    tr.append(name, el("td", r.kind === "plugin" ? "Plugin" : "Mod"), el("td", r.version),
              el("td", date(r.submittedAt)));
    const td = el("td");
    const b = el("button", r.own ? "Voir (la vôtre)" : "Relire", r.own ? "ghost" : "");
    b.addEventListener("click", () => openDetail(r.id));
    td.append(b);
    tr.append(td);
    tb.append(tr);
  }
  $("#queue-empty").hidden = rows.length > 0;
}

async function openDetail(id) {
  let d, diff;
  try {
    [d, diff] = await Promise.all([
      api("GET", `/v1/review/submissions/${id}`),
      api("GET", `/v1/review/submissions/${id}/source-diff`),
    ]);
  } catch (e) { return status(e.message, true); }
  current = d;
  $("#queue-view").hidden = true;
  $("#detail").hidden = false;
  $("#d-title").textContent = `${d.slug} ${d.version} (${d.kind === "plugin" ? "plugin" : "mod"})`;
  $("#d-own").hidden = !d.own;
  $("#approve").disabled = $("#reject").disabled = d.own || d.status !== "pending";
  $("#d-note").value = "";

  const perms = $("#d-perms");
  perms.replaceChildren();
  const list = Array.isArray(d.manifest.permissions) ? d.manifest.permissions : [];
  if (!list.length) perms.append(el("li", "aucune", "muted"));
  for (const p of list) {
    const li = el("li", `${p} — ${PERM_TEXT[p] || "permission inconnue"}`);
    if (RISKY.has(p) || !PERM_TEXT[p]) li.className = "perm-risky";
    perms.append(li);
  }
  const svc = d.manifest.service;
  $("#d-service").textContent = svc ? [svc.run, ...(svc.args || [])].join(" ") : "(aucun)";
  $("#d-manifest").textContent = JSON.stringify(d.manifest, null, 2);

  const files = $("#d-files");
  files.replaceChildren();
  const ch = d.changesSincePrevious;
  $("#d-prev").textContent = ch ? `— depuis ${ch.previousVersion}` : "— première version";
  const mark = (n) => !ch ? "" : ch.added.includes(n) ? "added" : ch.changed.includes(n) ? "changed" : "";
  for (const n of Object.keys(d.files).sort()) {
    const m = mark(n);
    files.append(el("li", (m === "added" ? "+ " : m === "changed" ? "~ " : "  ") + n, m));
  }
  if (ch) for (const n of ch.removed) files.append(el("li", "- " + n, "removed"));

  const pre = $("#d-diff");
  pre.replaceChildren();
  for (const line of String(diff).split("\n")) {
    const c = line.startsWith("@@") ? "h" : line.startsWith("+") && !line.startsWith("+++") ? "a"
            : line.startsWith("-") && !line.startsWith("---") ? "d" : "";
    pre.append(el("span", line, c));
  }
  status("");
  window.scrollTo(0, 0);
}

async function decide(approve) {
  const note = $("#d-note").value.trim();
  if (!approve && !note) return status("Un refus doit être motivé.", true);
  if (approve && !confirm(`Approuver et SIGNER ${current.slug} ${current.version} ?`)) return;
  try {
    await api("POST", `/v1/review/submissions/${current.id}/${approve ? "approve" : "reject"}`, { note });
    status(approve ? "Approuvé et signé." : "Refusé.");
    loadQueue();
  } catch (e) { status(e.message, true); }
}

$("#approve").addEventListener("click", () => decide(true));
$("#reject").addEventListener("click", () => decide(false));
$("#back").addEventListener("click", loadQueue);

// --- Onglets et administration -------------------------------------------------

for (const t of document.querySelectorAll(".tab")) {
  t.addEventListener("click", () => {
    for (const o of document.querySelectorAll(".tab")) o.classList.toggle("active", o === t);
    const admin = t.dataset.tab === "admin";
    $("#admin-view").hidden = !admin;
    $("#detail").hidden = true;
    $("#queue-view").hidden = admin;
    if (!admin) loadQueue();
  });
}

async function kill(on) {
  const note = $("#kill-note").value.trim();
  if (on && !confirm("Arrêter TOUS les plugins sur tous les launchers ?")) return;
  try {
    const r = await api("POST", "/v1/admin/killswitch", { on, note });
    status(`Liste n°${r.seq} publiée : kill-switch ${on ? "ACTIF" : "levé"}.`);
  } catch (e) { status(e.message, true); }
}
$("#kill-on").addEventListener("click", () => kill(true));
$("#kill-off").addEventListener("click", () => kill(false));

$("#revoke-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  const id = f.get("id"), version = f.get("version");
  if (!confirm(`Révoquer ${id} ${version} ? Le plugin sera arrêté partout.`)) return;
  try {
    const r = await api("POST", "/v1/admin/revoke", { releases: [{ id, version }], note: f.get("note") });
    status(`Liste n°${r.seq} publiée : ${id} ${version} révoqué.`);
    ev.target.reset();
  } catch (e) { status(e.message, true); }
});

start();
