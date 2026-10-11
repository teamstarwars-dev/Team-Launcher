"""Télémétrie des plugins : agrégée, anonyme, bornée."""

from __future__ import annotations

from conftest import Account, plugin_zip, set_role, submit
from sqlalchemy import inspect

from app import models as m


def _published(env):
    c = env["client"]
    dev, rv = Account(c, "dev@ex.org"), Account(c, "rv@ex.org")
    set_role(env, rv, reviewer=True)
    dev.post("/v1/projects", json={"kind": "plugin", "slug": "hello", "name": "Hello"})
    sid = submit(dev, "hello", "1.0.0", plugin_zip()).json()["id"]
    rv.post(f"/v1/review/submissions/{sid}/approve", json={})
    return dev, rv


def _send(c, events):
    return c.post("/v1/public/telemetry",
                  json={"v": 1, "launcher": "8.0.0", "os": "windows", "events": events})


def test_counts_only_published_versions_and_known_kinds(env):
    _published(env)
    c = env["client"]
    r = _send(c, [
        {"plugin": "hello", "version": "1.0.0", "kind": "rate_abuse", "count": 2},
        {"plugin": "hello", "version": "1.0.0", "kind": "rate_abuse", "count": 3},
        {"plugin": "hello", "version": "9.9.9", "kind": "rate_abuse", "count": 5},   # non publiée
        {"plugin": "inconnu", "version": "1.0", "kind": "denied", "count": 5},       # inconnu
        {"plugin": "hello", "version": "1.0.0", "kind": "texte libre", "count": 1},  # raison inconnue
    ])
    assert r.status_code == 204
    with env["db"]() as s:
        rows = s.query(m.TelemetryDaily).all()
    assert [(x.plugin, x.version, x.kind, x.count) for x in rows] == \
        [("hello", "1.0.0", "rate_abuse", 5)]


def test_nothing_identifying_is_stored(env):
    # La table n'a QUE ces colonnes : pas d'adresse, pas d'identifiant.
    cols = {col["name"] for col in inspect(env["db"]().get_bind()).get_columns("telemetry_daily")}
    assert cols == {"day", "plugin", "version", "kind", "count"}


def test_rate_limited_per_address(env):
    c = env["client"]
    codes = [_send(c, []).status_code for _ in range(13)]
    assert codes[:12] == [204] * 12 and codes[12] == 429


def test_strict_validation(env):
    c = env["client"]
    assert _send(c, [{"plugin": "x" * 65, "version": "1", "kind": "denied", "count": 1}]).status_code == 422
    assert _send(c, [{"plugin": "a", "version": "1", "kind": "denied", "count": 0}]).status_code == 422
    assert _send(c, [{"plugin": "a", "version": "1", "kind": "denied", "count": 1}] * 201).status_code == 422
    assert c.post("/v1/public/telemetry", json={"v": 2, "events": []}).status_code == 422


def test_admin_summary(env):
    dev, rv = _published(env)
    _send(env["client"], [{"plugin": "hello", "version": "1.0.0", "kind": "denied", "count": 4}])
    assert rv.get("/v1/admin/telemetry").status_code == 403  # relecteur, pas admin
    ad = Account(env["client"], "admin@ex.org")
    set_role(env, ad, admin=True)
    r = ad.get("/v1/admin/telemetry?days=7").json()
    assert r["rows"] == [{"plugin": "hello", "version": "1.0.0", "kind": "denied", "count": 4}]
