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
  if (!me.mfaOk) {
    $("#mfa-setup").hidden = true;
    // Windows Hello / clé FIDO proposé en premier quand le compte en a une.
    $("#mfa-key").hidden = !(me.webauthn > 0 && window.PublicKeyCredential);
    return show("mfa");
  }
  // Chacun voit les espaces de ses droits : tout compte (2FA) a ses
  // projets ; la relecture et l'administration selon les rôles.
  $("#tab-queue").hidden = !me.reviewer;
  $("#tab-admin").hidden = !me.admin;
  show("app");
  selectTab(me.reviewer ? "queue" : "projects");
}

// QR code dessiné module par module sur le canvas : aucune image data: (la
// CSP l'interdit) et aucun HTML inséré. Fond blanc et marge de 4 modules,
// sans lesquels certains téléphones ne lisent pas le code en mode sombre.
function drawQr(canvas, text) {
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (!text || typeof qrcode !== "function") return;
  const qr = qrcode(0, "M");
  qr.addData(text);
  qr.make();
  const n = qr.getModuleCount(), quiet = 4;
  const cell = Math.floor(canvas.width / (n + 2 * quiet));
  const offset = Math.floor((canvas.width - cell * n) / 2);
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = "#000000";
  for (let r = 0; r < n; r++)
    for (let c = 0; c < n; c++)
      if (qr.isDark(r, c)) ctx.fillRect(offset + c * cell, offset + r * cell, cell, cell);
}

async function setupMfa() {
  const r = await api("POST", "/v1/auth/totp/setup");
  drawQr($("#mfa-qr"), r.uri);
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
    drawQr($("#mfa-qr"), ""); // le secret ne reste pas affiché
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

function selectTab(name) {
  for (const o of document.querySelectorAll(".tab")) o.classList.toggle("active", o.dataset.tab === name);
  $("#projects-view").hidden = name !== "projects";
  $("#queue-view").hidden = name !== "queue";
  $("#admin-view").hidden = name !== "admin";
  $("#security-view").hidden = name !== "security";
  $("#detail").hidden = true;
  if (name === "security") loadKeys();
  if (name === "queue") loadQueue();
  if (name === "projects") loadProjects();
}

for (const t of document.querySelectorAll(".tab"))
  t.addEventListener("click", () => selectTab(t.dataset.tab));

// --- Espace développeur ---------------------------------------------------------

const STATUS_TEXT = { pending: "en attente de relecture", approved: "approuvée et signée",
                      rejected: "refusée", revoked: "révoquée" };

async function apiForm(path, form) {
  const r = await fetch(path, { method: "POST", headers: { Authorization: "Bearer " + token }, body: form });
  let data = null;
  try { data = await r.json(); } catch { }
  if (!r.ok) {
    const d = data && data.detail;
    throw new Error(typeof d === "string" ? d : `erreur ${r.status}`);
  }
  return data;
}

$("#project-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  try {
    await api("POST", "/v1/projects", { kind: f.get("kind"), slug: String(f.get("slug")).trim(),
                                         name: String(f.get("name")).trim() });
    ev.target.reset();
    status("Projet créé. Envoyez maintenant sa première version.");
    loadProjects();
  } catch (e) { status(e.message, true); }
});

async function loadProjects() {
  let projects;
  try { projects = await api("GET", "/v1/projects/mine"); } catch (e) { return status(e.message, true); }
  const list = $("#projects-list");
  list.replaceChildren();
  $("#projects-empty").hidden = projects.length > 0;
  for (const p of projects) list.append(await projectCard(p));
}

async function projectCard(p) {
  const card = el("div", undefined, "card proj");
  const h = el("h3");
  h.append(el("span", p.name), el("span", `${p.kind === "plugin" ? "Plugin" : "Mod"} · ${p.slug}`, "muted"));
  if (p.banned) h.append(el("span", "suspendu", "st-rejected"));
  card.append(h);

  let rows = [];
  try { rows = await api("GET", `/v1/projects/${p.kind}/${p.slug}/submissions`); } catch { }
  if (rows.length) {
    const t = el("table", undefined, "versions");
    const head = el("tr");
    for (const c of ["Version", "État", "Envoyée le", "Note de relecture"]) head.append(el("th", c));
    t.append(head);
    for (const r of rows) {
      const tr = el("tr");
      tr.append(el("td", r.version), el("td", STATUS_TEXT[r.status] || r.status, "st-" + r.status),
                el("td", date(r.submittedAt)), el("td", r.note || "—"));
      t.append(tr);
    }
    card.append(t);
  } else {
    card.append(el("p", "Aucune version envoyée.", "muted"));
  }

  // Envoi d'une version : archive + sources, toujours les deux.
  const form = el("form", undefined, "upload");
  const ver = el("input"); ver.name = "version"; ver.required = true; ver.maxLength = 32;
  ver.placeholder = "version (ex. 1.0.0)";
  const lv = el("label", "Version"); lv.append(ver);
  const bin = el("input"); bin.type = "file"; bin.name = "binary"; bin.accept = ".zip"; bin.required = true;
  const lb = el("label", p.kind === "plugin" ? "Archive du plugin (.zip, avec plugin.json)"
                                             : "Archive du mod (.zip, des .jar à la racine)");
  lb.append(bin);
  const src = el("input"); src.type = "file"; src.name = "source"; src.accept = ".zip"; src.required = true;
  const ls = el("label", "Sources (.zip)"); ls.append(src);
  const send = el("button", "Envoyer cette version");
  form.append(lv, lb, ls, send);
  form.addEventListener("submit", async (ev) => {
    ev.preventDefault();
    send.disabled = true;
    status("Envoi en cours…");
    try {
      await apiForm(`/v1/projects/${p.kind}/${p.slug}/submissions`, new FormData(form));
      status(`Version ${ver.value} envoyée : l'équipe est prévenue et va la relire.`);
      loadProjects();
    } catch (e) {
      status(e.message, true);
      send.disabled = false;
    }
  });
  card.append(form);
  return card;
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


// --- WebAuthn : Windows Hello, clés de sécurité ---------------------------------
//
// Le serveur envoie ses options en JSON (champs binaires en base64url) ; le
// navigateur veut des ArrayBuffer. Et dans l'autre sens pour la réponse.

function b64uToBuf(s) {
  const b = atob(s.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((s.length + 3) % 4));
  return Uint8Array.from(b, (c) => c.charCodeAt(0)).buffer;
}

function bufToB64u(buf) {
  let s = "";
  for (const b of new Uint8Array(buf)) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function creationOptions(o) {
  return { ...o, challenge: b64uToBuf(o.challenge), user: { ...o.user, id: b64uToBuf(o.user.id) },
           excludeCredentials: (o.excludeCredentials || []).map((c) => ({ ...c, id: b64uToBuf(c.id) })) };
}

function requestOptions(o) {
  return { ...o, challenge: b64uToBuf(o.challenge),
           allowCredentials: (o.allowCredentials || []).map((c) => ({ ...c, id: b64uToBuf(c.id) })) };
}

function credentialJson(cred) {
  const r = cred.response, out = { id: cred.id, rawId: bufToB64u(cred.rawId), type: cred.type,
                                    response: { clientDataJSON: bufToB64u(r.clientDataJSON) },
                                    clientExtensionResults: cred.getClientExtensionResults() };
  if (r.attestationObject) {
    out.response.attestationObject = bufToB64u(r.attestationObject);
    if (r.getTransports) out.response.transports = r.getTransports();
  } else {
    out.response.authenticatorData = bufToB64u(r.authenticatorData);
    out.response.signature = bufToB64u(r.signature);
    out.response.userHandle = r.userHandle ? bufToB64u(r.userHandle) : null;
  }
  return out;
}

$("#mfa-key-btn").addEventListener("click", async () => {
  try {
    const o = await api("POST", "/v1/auth/webauthn/login/options");
    const cred = await navigator.credentials.get({ publicKey: requestOptions(o) });
    await api("POST", "/v1/auth/webauthn/login/verify", { credential: credentialJson(cred) });
    status("");
    start();
  } catch (e) {
    status(e.name === "NotAllowedError" ? "Opération annulée ou délai dépassé." : e.message, true);
  }
});

async function loadKeys() {
  let keys;
  try { keys = await api("GET", "/v1/auth/webauthn/credentials"); } catch (e) { return status(e.message, true); }
  const ul = $("#keys");
  ul.replaceChildren();
  $("#keys-empty").hidden = keys.length > 0;
  for (const k of keys) {
    const li = el("li");
    const info = el("span");
    info.append(el("strong", k.name), " ",
                el("span", `ajoutée le ${date(k.createdAt)}` +
                           (k.lastUsedAt ? `, utilisée le ${date(k.lastUsedAt)}` : ", jamais utilisée"), "muted"));
    const rm = el("button", "Retirer", "ghost");
    rm.addEventListener("click", async () => {
      if (!confirm(`Retirer la clé « ${k.name} » ?`)) return;
      try { await api("POST", `/v1/auth/webauthn/credentials/${k.id}/delete`); loadKeys(); }
      catch (e) { status(e.message, true); }
    });
    li.append(info, rm);
    ul.append(li);
  }
}

$("#key-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  if (!window.PublicKeyCredential) return status("Ce navigateur ne gère pas les clés de sécurité.", true);
  const name = String(new FormData(ev.target).get("name")).trim();
  try {
    const o = await api("POST", "/v1/auth/webauthn/register/options");
    const cred = await navigator.credentials.create({ publicKey: creationOptions(o) });
    await api("POST", "/v1/auth/webauthn/register/verify", { name, credential: credentialJson(cred) });
    ev.target.reset();
    status(`Clé « ${name} » ajoutée. Vous pourrez l'utiliser à la prochaine connexion.`);
    loadKeys();
  } catch (e) {
    status(e.name === "NotAllowedError" ? "Opération annulée ou délai dépassé." :
           e.name === "InvalidStateError" ? "Cette clé est déjà enregistrée sur ce compte." : e.message, true);
  }
});
