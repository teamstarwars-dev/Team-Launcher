"""Point d'entrée : uvicorn app.main:app

Toute la configuration vient de l'environnement (voir config.py). Sans
elle, le serveur refuse de démarrer.
"""

from __future__ import annotations

from . import config
from .api import create_app
from .db import Base, make_engine, make_session_factory
from .signing import Signer


def build():
    settings = config.load()
    engine = make_engine(settings.database_url)
    Base.metadata.create_all(engine)
    signer = Signer.load(settings.signing_key_path, settings.signing_cert_path)
    return create_app(settings, make_session_factory(engine), signer)


app = build()
