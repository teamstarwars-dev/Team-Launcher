"""Tables de la marketplace (plugins et mods).

Horodatages en secondes Unix (entiers) : aucune ambiguïté de fuseau, et
c'est le format des documents signés que vérifie le launcher.
"""

from __future__ import annotations

from sqlalchemy import BigInteger, Boolean, ForeignKey, Integer, String, Text, UniqueConstraint
from sqlalchemy.orm import Mapped, mapped_column

from .db import Base

# MariaDB : InnoDB pour les clés étrangères et les transactions, utf8mb4
# pour l'Unicode complet (le « utf8 » historique de MySQL s'arrête à 3
# octets et tronque silencieusement les emojis).
_TABLE = {"mysql_engine": "InnoDB", "mysql_charset": "utf8mb4"}


class Developer(Base):
    __tablename__ = "developers"
    __table_args__ = _TABLE

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    # Compte Team Launcher unique (V8, section 6) : un joueur s'inscrit par
    # pseudo (amis, messages, vocal, chat) ; un développeur de la
    # marketplace par adresse. Les deux peuvent coexister sur un compte.
    email: Mapped[str | None] = mapped_column(String(254), unique=True, nullable=True)
    username: Mapped[str | None] = mapped_column(String(24), nullable=True)
    # Pseudo en minuscules : unicité insensible à la casse, quelle que soit
    # la collation de la base.
    username_key: Mapped[str | None] = mapped_column(String(24), unique=True, nullable=True)
    presence: Mapped[str] = mapped_column(String(8), default="online")
    display_name: Mapped[str] = mapped_column(String(64))
    password_hash: Mapped[str] = mapped_column(String(255))
    # Secret TOTP CHIFFRÉ (SecretBox). Vide tant que la 2FA n'est pas posée.
    totp_secret_enc: Mapped[str] = mapped_column(String(255), default="")
    totp_enabled: Mapped[bool] = mapped_column(Boolean, default=False)
    # Dernier pas TOTP accepté : empêche de rejouer un code déjà utilisé
    # pendant sa fenêtre de validité.
    totp_last_step: Mapped[int] = mapped_column(BigInteger, default=0)
    is_reviewer: Mapped[bool] = mapped_column(Boolean, default=False)
    is_admin: Mapped[bool] = mapped_column(Boolean, default=False)
    failed_logins: Mapped[int] = mapped_column(Integer, default=0)
    locked_until: Mapped[int] = mapped_column(BigInteger, default=0)
    created_at: Mapped[int] = mapped_column(BigInteger)


class Session(Base):
    __tablename__ = "sessions"
    __table_args__ = _TABLE

    # Empreinte SHA-256 du jeton : une fuite de la base ne livre aucune
    # session utilisable.
    token_hash: Mapped[str] = mapped_column(String(64), primary_key=True)
    developer_id: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"))
    created_at: Mapped[int] = mapped_column(BigInteger)
    expires_at: Mapped[int] = mapped_column(BigInteger)
    # Le mot de passe seul ouvre une session « à moitié » : rien de
    # sensible n'est permis tant que le second facteur n'a pas été donné.
    mfa_ok: Mapped[bool] = mapped_column(Boolean, default=False)
    # Dernière requête authentifiée : sert de battement de présence.
    last_seen: Mapped[int] = mapped_column(BigInteger, default=0)


class Project(Base):
    __tablename__ = "projects"
    __table_args__ = (UniqueConstraint("kind", "slug"), _TABLE)

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    kind: Mapped[str] = mapped_column(String(16))  # "plugin" | "mod"
    # Pour un plugin : c'est le nom de son dossier, donc son identifiant
    # dans le launcher — mêmes règles que le plugin host.
    slug: Mapped[str] = mapped_column(String(64))
    name: Mapped[str] = mapped_column(String(128))
    owner_id: Mapped[int] = mapped_column(ForeignKey("developers.id"))
    banned: Mapped[bool] = mapped_column(Boolean, default=False)
    created_at: Mapped[int] = mapped_column(BigInteger)


class Submission(Base):
    """Une version soumise d'un projet."""

    __tablename__ = "submissions"
    __table_args__ = (UniqueConstraint("project_id", "version"), _TABLE)

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    project_id: Mapped[int] = mapped_column(ForeignKey("projects.id"))
    version: Mapped[str] = mapped_column(String(32))
    uploaded_by: Mapped[int] = mapped_column(ForeignKey("developers.id"))
    created_at: Mapped[int] = mapped_column(BigInteger)
    # pending -> approved | rejected ; approved -> revoked
    status: Mapped[str] = mapped_column(String(16), default="pending")
    binary_sha512: Mapped[str] = mapped_column(String(128))
    source_sha512: Mapped[str] = mapped_column(String(128))
    # Liste des fichiers du binaire et leur SHA-512, calculée à la
    # réception — c'est EXACTEMENT ce qui sera signé, et ce que les
    # relecteurs comparent d'une version à l'autre.
    files_json: Mapped[str] = mapped_column(Text)
    manifest_json: Mapped[str] = mapped_column(Text)
    reviewer_id: Mapped[int | None] = mapped_column(ForeignKey("developers.id"), nullable=True)
    review_note: Mapped[str] = mapped_column(Text, default="")
    reviewed_at: Mapped[int] = mapped_column(BigInteger, default=0)
    # signature.json, produit à l'approbation.
    signature_json: Mapped[str] = mapped_column(Text, default="")


class AuditEvent(Base):
    """Journal des actions sensibles. On y ajoute, on n'y modifie jamais."""

    __tablename__ = "audit_events"
    __table_args__ = _TABLE

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    at: Mapped[int] = mapped_column(BigInteger)
    actor_id: Mapped[int | None] = mapped_column(ForeignKey("developers.id"), nullable=True)
    action: Mapped[str] = mapped_column(String(48))
    target: Mapped[str] = mapped_column(String(128), default="")
    note: Mapped[str] = mapped_column(Text, default="")


class Revocation(Base):
    """Une liste de révocation publiée. La plus haute `seq` est en vigueur."""

    __tablename__ = "revocations"
    __table_args__ = _TABLE

    seq: Mapped[int] = mapped_column(BigInteger, primary_key=True, autoincrement=False)
    issued_at: Mapped[int] = mapped_column(BigInteger)
    kill_all: Mapped[bool] = mapped_column(Boolean, default=False)
    # Contenu en clair (pour l'administration) et document signé servi
    # tel quel au launcher.
    content_json: Mapped[str] = mapped_column(Text)
    document: Mapped[str] = mapped_column(Text)


class TelemetryDaily(Base):
    """Télémétrie des plugins, AGRÉGÉE par jour.

    Aucune adresse IP, aucun identifiant de poste, aucun événement
    individuel : seulement « tel jour, telle version de tel plugin a été
    arrêtée N fois pour telle raison ». C'est tout ce qu'il faut pour
    repérer un plugin qui dérape, et rien de plus.
    """

    __tablename__ = "telemetry_daily"
    __table_args__ = _TABLE

    day: Mapped[int] = mapped_column(Integer, primary_key=True)  # AAAAMMJJ
    plugin: Mapped[str] = mapped_column(String(64), primary_key=True)
    version: Mapped[str] = mapped_column(String(32), primary_key=True)
    kind: Mapped[str] = mapped_column(String(24), primary_key=True)
    count: Mapped[int] = mapped_column(BigInteger, default=0)


# --- Social (section 6) : reprise de l'ancienne API Node, même contrat ------


class Friendship(Base):
    """Relation entre deux comptes, paire ordonnée (user_a < user_b)."""

    __tablename__ = "friendships"
    __table_args__ = _TABLE

    user_a: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"),
                                        primary_key=True)
    user_b: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"),
                                        primary_key=True)
    status: Mapped[str] = mapped_column(String(10))  # pending | accepted | blocked
    requested_by: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"))
    created_at: Mapped[int] = mapped_column(BigInteger)


class DirectMessage(Base):
    __tablename__ = "direct_messages"
    __table_args__ = _TABLE

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    sender: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"),
                                        index=True)
    recipient: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"),
                                           index=True)
    body: Mapped[str] = mapped_column(Text)
    created_at: Mapped[int] = mapped_column(BigInteger)


class Call(Base):
    __tablename__ = "calls"
    __table_args__ = _TABLE

    id: Mapped[str] = mapped_column(String(16), primary_key=True)
    caller: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"))
    callee: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"))
    lobby_id: Mapped[str] = mapped_column(String(128))
    status: Mapped[str] = mapped_column(String(8))  # ringing | active | ended
    created_at: Mapped[int] = mapped_column(BigInteger)
    updated_at: Mapped[int] = mapped_column(BigInteger)


class UserEvent(Base):
    """File d'événements d'un compte, distribuée par long-polling."""

    __tablename__ = "user_events"
    __table_args__ = _TABLE

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"),
                                         index=True)
    type: Mapped[str] = mapped_column(String(32))
    payload: Mapped[str] = mapped_column(Text)
    created_at: Mapped[int] = mapped_column(BigInteger)
