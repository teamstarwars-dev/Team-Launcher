"""Mise à jour du schéma d'une base déjà en service.

`create_all` crée les tables qui manquent, mais n'ajoute jamais une colonne
à une table existante. Ce module complète, colonne par colonne, et
seulement en AJOUTANT : il ne supprime ni ne renomme jamais rien. Chaque
étape regarde l'état réel de la base avant d'agir : relancer est sans
effet.
"""

from __future__ import annotations

from sqlalchemy import inspect, text
from sqlalchemy.engine import Engine

from .db import Base

# (table, colonne, définition MySQL/MariaDB, définition SQLite)
_COLUMNS = [
    ("developers", "username", "VARCHAR(24) NULL", "VARCHAR(24)"),
    ("developers", "username_key", "VARCHAR(24) NULL", "VARCHAR(24)"),
    ("developers", "presence", "VARCHAR(8) NOT NULL DEFAULT 'online'",
     "VARCHAR(8) NOT NULL DEFAULT 'online'"),
    ("sessions", "last_seen", "BIGINT NOT NULL DEFAULT 0", "BIGINT NOT NULL DEFAULT 0"),
]


def upgrade(engine: Engine) -> list[str]:
    """Crée les tables manquantes puis ajoute les colonnes manquantes.
    Rend la liste des changements faits (vide si la base était à jour)."""
    done: list[str] = []
    Base.metadata.create_all(engine)
    mysql = engine.dialect.name in ("mysql", "mariadb")
    with engine.begin() as conn:
        insp = inspect(conn)
        tables = set(insp.get_table_names())
        for table, col, mysql_def, sqlite_def in _COLUMNS:
            if table not in tables:
                continue
            if col in {c["name"] for c in insp.get_columns(table)}:
                continue
            conn.execute(text(f"ALTER TABLE {table} ADD COLUMN {col} "
                              f"{mysql_def if mysql else sqlite_def}"))
            done.append(f"{table}.{col}")
        if mysql and "developers" in tables:
            # Un joueur n'a pas forcément d'adresse : l'adresse devient
            # facultative (elle reste unique quand elle est renseignée).
            email = next(c for c in insp.get_columns("developers") if c["name"] == "email")
            if not email["nullable"]:
                conn.execute(text("ALTER TABLE developers MODIFY email VARCHAR(254) NULL"))
                done.append("developers.email facultative")
            uniques = {u["name"] for u in insp.get_unique_constraints("developers")}
            indexes = {i["name"] for i in insp.get_indexes("developers")}
            if "uq_developers_username_key" not in uniques | indexes:
                conn.execute(text("ALTER TABLE developers "
                                  "ADD UNIQUE KEY uq_developers_username_key (username_key)"))
                done.append("developers.username_key unique")
    return done
