"""Alerte Discord de l'équipe à chaque nouvelle soumission."""

from __future__ import annotations

import dataclasses
import json
import threading

import pytest
from conftest import Account, plugin_zip, submit

from app import config, notify
from app.api import create_app
from app.signing import Signer

HOOK = "https://discord.com/api/webhooks/123456789/abcDEF_ghi-JKL"


@pytest.fixture()
def captured(monkeypatch):
    sent, done = [], threading.Event()

    def fake(url, body):
        sent.append((url, json.loads(body)))
        done.set()

    monkeypatch.setattr(notify, "_sender", fake)
    return sent, done


def _client_with_hook(env, hook):
    settings = dataclasses.replace(env["settings"], discord_webhook=hook,
                                   public_url="https://panel.example.org")
    from fastapi.testclient import TestClient
    return TestClient(create_app(settings, env["db"], Signer.load(env["key_path"], env["cert_path"])))


def test_alert_sent_on_submission(env, captured):
    sent, done = captured
    c = _client_with_hook(env, HOOK)
    dev = Account(c, "dev@ex.org")
    dev.post("/v1/projects", json={"kind": "plugin", "slug": "hello",
                                   "name": "Hello @everyone [lien](https://x.y)"})
    assert submit(dev, "hello", "1.0.0", plugin_zip()).status_code == 201
    assert done.wait(5)
    url, body = sent[0]
    assert url == HOOK
    assert body["allowed_mentions"] == {"parse": []}  # personne n'est mentionné
    text = body["content"]
    assert "Nouveau plugin en attente" in text and "`hello`" in text and "`1.0.0`" in text
    assert "https://panel.example.org/panel" in text
    assert "](" not in text and "https://x.y" not in text  # pas de lien injecté


def test_no_alert_without_webhook(env, captured):
    sent, done = captured
    dev = Account(env["client"], "dev@ex.org")
    dev.post("/v1/projects", json={"kind": "plugin", "slug": "hello", "name": "Hello"})
    assert submit(dev, "hello", "1.0.0", plugin_zip()).status_code == 201
    assert not done.wait(0.5) and sent == []


def test_webhook_url_validated(monkeypatch):
    assert notify.valid_webhook(HOOK)
    for bad in ("http://discord.com/api/webhooks/1/a", "https://evil.example/api/webhooks/1/a",
                "https://discord.com.evil.example/api/webhooks/1/a", ""):
        assert not notify.valid_webhook(bad)
    monkeypatch.setenv("TL_MARKET_DISCORD_WEBHOOK", "https://evil.example/hook")
    with pytest.raises(config.ConfigError):
        config._webhook()
