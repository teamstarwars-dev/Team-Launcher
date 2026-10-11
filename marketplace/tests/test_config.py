"""Mot de passe de la base sur sa propre ligne, pris tel quel."""

from __future__ import annotations

from pathlib import Path

from sqlalchemy.engine import make_url

from app import config
from run import load_dotenv

HARD = "aB3@x:y/z?w#v%40+=^ !"


def test_raw_password_survives_every_special_character(tmp_path, monkeypatch):
    for k in ("TL_MARKET_DB_URL", "TL_MARKET_DB_PASSWORD"):
        monkeypatch.delenv(k, raising=False)
    env = tmp_path / ".env"
    env.write_text(
        "TL_MARKET_DB_URL=mysql+pymysql://u206_X@82.66.173.236:3306/s206_db?charset=utf8mb4\n"
        f"TL_MARKET_DB_PASSWORD={HARD}\n", encoding="utf-8")
    load_dotenv(env)
    u = make_url(config.database_url())
    assert u.password == HARD
    assert (u.username, u.host, u.port, u.database) == ("u206_X", "82.66.173.236", 3306, "s206_db")
    assert u.query["charset"] == "utf8mb4"


def test_url_alone_still_works(monkeypatch):
    monkeypatch.setenv("TL_MARKET_DB_URL", "mysql+pymysql://u:p%40ss@h:3306/b")
    monkeypatch.delenv("TL_MARKET_DB_PASSWORD", raising=False)
    assert make_url(config.database_url()).password == "p@ss"
