"""Clés WebAuthn : Windows Hello, Touch ID, clés de sécurité FIDO2.

Table à part plutôt que dans models.py : elle ne dépend de rien d'autre que
du compte, et s'ajoute à une base existante sans toucher aux autres.
"""

from __future__ import annotations

from sqlalchemy import BigInteger, ForeignKey, Integer, String, Text
from sqlalchemy.orm import Mapped, mapped_column

from .db import Base

_TABLE = {"mysql_engine": "InnoDB", "mysql_charset": "utf8mb4"}


class WebAuthnCredential(Base):
    __tablename__ = "webauthn_credentials"
    __table_args__ = _TABLE

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    developer_id: Mapped[int] = mapped_column(ForeignKey("developers.id", ondelete="CASCADE"),
                                              index=True)
    # Identifiant de la clé (base64url), unique au monde par construction.
    credential_id: Mapped[str] = mapped_column(String(255), unique=True)
    # Clé PUBLIQUE COSE (base64url) : rien de secret n'est stocké ici.
    public_key: Mapped[str] = mapped_column(Text)
    # Compteur de signatures : une clé clonée finit par rejouer un compteur
    # déjà vu, et elle est alors refusée.
    sign_count: Mapped[int] = mapped_column(BigInteger, default=0)
    name: Mapped[str] = mapped_column(String(64))
    created_at: Mapped[int] = mapped_column(BigInteger)
    last_used_at: Mapped[int] = mapped_column(BigInteger, default=0)
