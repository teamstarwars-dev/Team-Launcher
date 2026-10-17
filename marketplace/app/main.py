"""Point d'entrée : uvicorn app.main:app

Toute la configuration vient de l'environnement (voir config.py). Sans
elle, le serveur refuse de démarrer.
"""

from __future__ import annotations

from . import config, social_api
from .api import create_app
from .db import make_engine, make_session_factory
from .migrate import upgrade
from .signing import Signer


def build():
    settings = config.load()
    engine = make_engine(settings.database_url)
    for change in upgrade(engine):  # tables et colonnes manquantes, sans rien effacer
        print(f"Schéma mis à jour : {change}", flush=True)
    signer = Signer.load(settings.signing_key_path, settings.signing_cert_path)
    session_factory = make_session_factory(engine)
    app = create_app(settings, session_factory, signer)
    # API sociale (amis, messages, appels, events) sur le prefixe /api : c'est
    # elle que le launcher appelle via tlApiUrl. Montee ici pour qu'elle soit
    # servie par la meme application que le marketplace.
    social_api.install(app, session_factory)
    return app


app = build()
