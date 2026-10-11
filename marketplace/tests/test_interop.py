"""Le serveur produit-il toujours EXACTEMENT ce que le launcher a appris à
vérifier ? cpp/tests/interop est vérifié par cpp/tests/test_signing.cpp ;
ici on exige que le serveur le reproduise à l'octet près."""

from __future__ import annotations

from pathlib import Path

from tools.interop_fixture import build

FIXTURE = Path(__file__).resolve().parents[2] / "cpp" / "tests" / "interop"


def test_fixture_is_reproduced_byte_for_byte():
    expected = build()
    on_disk = {p.relative_to(FIXTURE).as_posix(): p.read_bytes()
               for p in FIXTURE.rglob("*") if p.is_file() and p.name != ".gitattributes"}
    assert set(on_disk) == set(expected), "régénérez : python -m tools.interop_fixture"
    for rel, data in expected.items():
        assert on_disk[rel] == data, f"{rel} diffère : le format a dérivé"
