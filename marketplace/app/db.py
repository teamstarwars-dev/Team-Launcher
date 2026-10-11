"""Accès à la base (MariaDB en production)."""

from __future__ import annotations

from sqlalchemy import create_engine
from sqlalchemy.engine import Engine
from sqlalchemy.orm import DeclarativeBase, sessionmaker


class Base(DeclarativeBase):
    pass


def make_engine(url: str) -> Engine:
    kwargs: dict = {"future": True}
    if url.startswith("mysql"):
        # MariaDB coupe les connexions inactives ; pool_pre_ping les
        # renouvelle au lieu de faire échouer la première requête après une
        # pause. pool_recycle reste sous le wait_timeout par défaut (8 h).
        kwargs.update(pool_pre_ping=True, pool_recycle=3600)
    return create_engine(url, **kwargs)


def make_session_factory(engine: Engine) -> sessionmaker:
    return sessionmaker(bind=engine, expire_on_commit=False, future=True)
