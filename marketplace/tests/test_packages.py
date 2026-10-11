"""Archives hostiles : rien de tout ça ne doit passer la porte."""

from __future__ import annotations

import io
import stat
import zipfile

import pytest
from conftest import Account, make_zip, plugin_zip, submit

from app.packages import PackageError, inspect_zip

LIM = {"max_files": 50, "max_unpacked": 1024 * 1024}


def _raw_zip(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for info, data in entries:
            z.writestr(info, data)
    return buf.getvalue()


@pytest.mark.parametrize("name", ["../evil.dll", "a/../../evil", "/etc/passwd", "C:/x.dll",
                                  "a\\b.dll", "a//b", "./x", "bad\x01name"])
def test_bad_paths(name):
    with pytest.raises(PackageError):
        inspect_zip(_raw_zip([(zipfile.ZipInfo(name), b"x"), ("plugin.json", "{}")]), **LIM)


def test_symlink_refused():
    info = zipfile.ZipInfo("lien")
    info.external_attr = (stat.S_IFLNK | 0o777) << 16
    with pytest.raises(PackageError, match="lien"):
        inspect_zip(_raw_zip([(info, "/etc/passwd"), ("plugin.json", "{}")]), **LIM)


def test_case_duplicates_refused():
    with pytest.raises(PackageError, match="doublon"):
        inspect_zip(make_zip({"A.dll": b"1", "a.dll": b"2", "plugin.json": "{}"}), **LIM)


def test_provided_signature_refused():
    with pytest.raises(PackageError, match="signature"):
        inspect_zip(make_zip({"signature.json": "{}", "plugin.json": "{}"}), **LIM)


def test_zip_bomb_refused():
    # 8 Mio de zéros compressés en quelques Kio : la taille réelle compte.
    with pytest.raises(PackageError, match="volumineuse"):
        inspect_zip(make_zip({"plugin.json": "{}", "bomb.bin": b"\0" * (8 * 1024 * 1024)}),
                    **LIM)


def test_too_many_files():
    files = {f"f{i}.txt": "x" for i in range(60)}
    files["plugin.json"] = "{}"
    with pytest.raises(PackageError, match="trop de fichiers"):
        inspect_zip(make_zip(files), **LIM)


def test_not_a_zip_and_empty():
    with pytest.raises(PackageError):
        inspect_zip(b"pas un zip", **LIM)
    with pytest.raises(PackageError):
        inspect_zip(make_zip({}), **LIM)


def test_manifest_required_and_valid():
    with pytest.raises(PackageError, match="absent"):
        inspect_zip(make_zip({"x.dll": b"1"}), **LIM)
    with pytest.raises(PackageError, match="illisible"):
        inspect_zip(make_zip({"plugin.json": "{pas du json"}), **LIM)
    with pytest.raises(PackageError, match="objet"):
        inspect_zip(make_zip({"plugin.json": "[1]"}), **LIM)


def test_hostile_archive_rejected_by_api(env):
    dev = Account(env["client"], "dev@ex.org")
    dev.post("/v1/projects", json={"kind": "plugin", "slug": "hello", "name": "Hello"})
    r = submit(dev, "hello", "1.0.0", plugin_zip(extra={"../../evil.dll": b"x"}))
    assert r.status_code == 400
    # Les SOURCES aussi sont inspectées : un relecteur n'ouvre pas de bombe.
    r = submit(dev, "hello", "1.0.0", plugin_zip(),
               make_zip({"bomb.bin": b"\0" * (32 * 1024 * 1024)}))
    assert r.status_code == 400


def test_upload_size_cap(env):
    import os
    dev = Account(env["client"], "dev@ex.org")
    dev.post("/v1/projects", json={"kind": "plugin", "slug": "hello", "name": "Hello"})
    big = plugin_zip(extra={"big.bin": os.urandom(5 * 1024 * 1024)})
    assert submit(dev, "hello", "1.0.0", big).status_code == 413
