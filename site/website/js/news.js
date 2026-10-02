/**
 * news.js — Charge automatiquement les actualités du launcher
 * et les affiche sur le site web.
 *
 * Source : le même JSON utilisé par le launcher (URL configurable).
 * Format : [{ "title": "...", "date": "...", "tag": "...", "text": "..." }]
 */

const NEWS_URL = "https://raw.githubusercontent.com/teamstarwars-dev/Team-Launcher/master/site/maquette/news.json";
const MAX_ITEMS = 5;

const TAG_COLORS = {
    "MAJ": "#6fbf3f", "UPDATE": "#6fbf3f", "VERSION": "#6fbf3f",
    "NOUVEAU": "#4aa3bf", "NEW": "#4aa3bf", "FEATURE": "#4aa3bf",
    "FIX": "#e0a03c", "CORRECTION": "#e0a03c", "BUG": "#e0a03c",
    "EVENT": "#aa7ddc", "ÉVÉNEMENT": "#aa7ddc", "EVENEMENT": "#aa7ddc",
    "IMPORTANT": "#f2555a", "URGENT": "#f2555a"
};

function getTagColor(tag) {
    const t = (tag || "").trim().toUpperCase();
    return TAG_COLORS[t] || "#9aa3ad";
}

function formatDate(raw) {
    if (!raw) return "";
    try {
        const d = new Date(raw);
        return d.toLocaleDateString("fr-FR", { day: "2-digit", month: "2-digit", year: "numeric" });
    } catch { return raw; }
}

function renderNews(items) {
    const container = document.getElementById("news-feed");
    if (!container) return;

    if (items.length === 0) {
        container.innerHTML = '<p class="news-empty">Aucune actualité pour le moment.</p>';
        return;
    }

    container.innerHTML = "";

    items.slice(0, MAX_ITEMS).forEach(item => {
        const card = document.createElement("div");
        card.className = "news-card";

        const hasTag = item.tag && item.tag.trim().length > 0;
        const tagColor = getTagColor(item.tag);

        card.innerHTML = `
            <div class="news-card-header">
                ${hasTag ? `<span class="news-tag" style="background:${tagColor}">${item.tag.toUpperCase()}</span>` : ""}
                <span class="news-title">${item.title || "Sans titre"}</span>
                <span class="news-date">${formatDate(item.date)}</span>
            </div>
            <div class="news-text">${(item.text || "").replace(/\n/g, "<br>")}</div>
        `;

        container.appendChild(card);
    });
}

async function loadNews() {
    const container = document.getElementById("news-feed");
    if (!container) return;

    try {
        const resp = await fetch(NEWS_URL, { cache: "no-store" });
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        const items = await resp.json();
        renderNews(Array.isArray(items) ? items : []);
    } catch (e) {
        // Fallback : essayer le cache local
        try {
            const resp2 = await fetch("news-cache.json", { cache: "no-store" });
            if (resp2.ok) {
                const items2 = await resp2.json();
                renderNews(Array.isArray(items2) ? items2 : []);
                return;
            }
        } catch { }
        container.innerHTML = '<p class="news-empty">Impossible de charger les actualités.</p>';
    }
}

document.addEventListener("DOMContentLoaded", loadNews);
