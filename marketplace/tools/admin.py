"""Gestion des rôles, sur le serveur (pas d'endpoint pour ça : nommer un
relecteur ne doit pas être possible depuis Internet).

    python -m tools.admin grant   dev@example.org reviewer
    python -m tools.admin revoke  dev@example.org admin
    python -m tools.admin reset-2fa dev@example.org
    python -m tools.admin set-password dev@example.org [motdepasse]
    python -m tools.admin list
"""

from __future__ import annotations

import secrets
import sys

from sqlalchemy import select

from app import config, models as m, security
from app.db import Base, make_engine, make_session_factory

ROLES = {"reviewer": "is_reviewer", "admin": "is_admin"}


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 1
    settings = config.load()
    engine = make_engine(settings.database_url)
    Base.metadata.create_all(engine)
    with make_session_factory(engine)() as s:
        if argv[0] == "list":
            for d in s.scalars(select(m.Developer).order_by(m.Developer.id)):
                roles = [r for r, col in ROLES.items() if getattr(d, col)]
                print(f"{d.id:5}  {d.email:40}  2FA={'oui' if d.totp_enabled else 'non':3}  "
                      f"{','.join(roles)}")
            return 0
        if len(argv) < 2:
            print(__doc__)
            return 1
        dev = s.scalar(select(m.Developer).where(m.Developer.email == argv[1].lower()))
        if dev is None:
            print("compte introuvable", file=sys.stderr)
            return 1
        if argv[0] in ("grant", "revoke") and len(argv) == 3 and argv[2] in ROLES:
            setattr(dev, ROLES[argv[2]], argv[0] == "grant")
            action = f"role_{argv[0]}_{argv[2]}"
        elif argv[0] == "reset-2fa":
            # Après vérification de l'identité HORS BANDE : le développeur
            # devra reposer sa 2FA à la prochaine connexion, et toutes ses
            # sessions tombent.
            dev.totp_enabled = False
            dev.totp_secret_enc = ""
            s.query(m.Session).filter(m.Session.developer_id == dev.id).delete()
            action = "totp_reset"
        elif argv[0] == "set-password":
            # Mot de passe donné en 3e argument, sinon on en génère un fort.
            # Toutes les sessions tombent ; le verrouillage éventuel est levé.
            new_password = argv[2] if len(argv) >= 3 else secrets.token_urlsafe(12)
            dev.password_hash = security.hash_password(new_password)
            dev.failed_logins = 0
            dev.locked_until = 0
            s.query(m.Session).filter(m.Session.developer_id == dev.id).delete()
            print(f"Nouveau mot de passe : {new_password}")
            action = "password_reset"
        else:
            print(__doc__)
            return 1
        s.add(m.AuditEvent(at=security.now(), actor_id=None, action=action, target=dev.email,
                           note="tools.admin"))
        s.commit()
        print("fait.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
