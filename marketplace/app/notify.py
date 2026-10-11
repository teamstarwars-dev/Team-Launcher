"""Alertes de l'équipe : un message Discord à chaque nouvelle soumission.

L'adresse du webhook vient du .env (TL_MARKET_DISCORD_WEBHOOK) : c'est un
secret, elle n'est jamais dans le code ni dans les journaux. Sans elle,
rien n'est envoyé.

Le message ne peut mentionner personne (allowed_mentions vide) : un nom de
projet contenant « @everyone » ne réveille pas tout le serveur. L'envoi
part dans un fil à part : une soumission n'attend jamais Discord, et un
Discord en panne ne fait jamais échouer une soumission.
"""

from __future__ import annotations

import json
import re
import threading
import urllib.request
from typing import Callable

WEBHOOK_RE = re.compile(r"^https://(discord\.com|discordapp\.com|ptb\.discord\.com|canary\.discord\.com)"
                        r"/api/webhooks/\d+/[A-Za-z0-9_-]+$")

# Remplaçable par les tests : (url, corps JSON) -> None
_sender: Callable[[str, bytes], None] | None = None


def valid_webhook(url: str) -> bool:
    return bool(WEBHOOK_RE.match(url or ""))


def _post(url: str, body: bytes) -> None:
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json",
                                          "User-Agent": "TeamLauncher-Marketplace"})
    try:
        urllib.request.urlopen(req, timeout=10).close()
    except Exception:
        pass  # une alerte perdue ne doit rien casser ; la file d'attente reste à jour


def _clean(text: str, limit: int = 100) -> str:
    # Pas de lien cliquable ni de mise en forme injectée par un nom libre.
    text = re.sub(r"[`*_~|>\[\]()]", "", text or "")
    return text.replace("://", ":/ /")[:limit]


def new_submission(webhook: str, panel_url: str, kind: str, slug: str, name: str,
                   version: str, developer: str) -> bool:
    """Prévient l'équipe. Rend False si aucun webhook valide n'est configuré."""
    if not valid_webhook(webhook):
        return False
    label = "Nouveau plugin" if kind == "plugin" else "Nouveau mod"
    lines = [f"**{label} en attente de relecture**",
             f"{_clean(name)} — `{slug}` version `{version}`",
             f"Soumis par {_clean(developer, 64)}"]
    if panel_url:
        lines.append(f"Relire : {panel_url.rstrip('/')}/review")
    body = json.dumps({"content": "\n".join(lines), "allowed_mentions": {"parse": []}}).encode()
    send = _sender or _post
    threading.Thread(target=send, args=(webhook, body), daemon=True).start()
    return True
