"""Regression tests for game material hidden in package dependencies."""
import gzip
import hashlib
import io
import json
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
import zipfile

from validate_package import validate


PYTHON_ARCHIVE = "EoWRecomp/eowgen/_internal/base_library.zip"
UNICORN_ARCHIVE = "EoWRecomp/licenses/generator/unicorn-2.1.4.tar.gz"


def zip_data(*entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        for name, data in entries:
            archive.writestr(name, data)
    return output.getvalue()


def tar_data(name, data, *, link=False):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        item = tarfile.TarInfo(name)
        item.size = len(data)
        if link:
            item.type = tarfile.SYMTYPE
            item.linkname = "../prod.keys"
        archive.addfile(item, io.BytesIO(data))
    return output.getvalue()


def validate_entries(*entries):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        archive = root / "package.zip"
        archive.write_bytes(zip_data(*entries))
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({"entries": [
            {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in entries]}), encoding="utf-8")
        return validate(archive, manifest)


class NestedArchiveTest(unittest.TestCase):
    def test_valid_dependency_archives(self):
        python = zip_data(("pkg/", b""), ("pkg/stdlib.pyc", b"python fixture"))
        unicorn = tar_data("unicorn-2.1.4/src/test.c", b"int dummy;")
        result = validate_entries((PYTHON_ARCHIVE, python), (UNICORN_ARCHIVE, unicorn))
        self.assertEqual(result["nested_archive_files"], {PYTHON_ARCHIVE: 1, UNICORN_ARCHIVE: 1})

    def test_zip_members_and_boundaries(self):
        ordinary = zip_data(("pkg/stdlib.pyc", b"python fixture"))
        directory = zip_data(("src/", b""), ("src/value", b"dummy"))
        link = zipfile.ZipInfo("pkg/link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        cases = (
            (zip_data(("pkg/prod.keys", b"dummy")), ValueError, "game material path"),
            (zip_data(("pkg/data.bin", b"PFS0payload")), ValueError, "game material signature"),
            (zip_data(("pkg/data.bin", ordinary)), ValueError, "unexpected archive"),
            (zip_data(("pkg/value", b"one"), ("PKG/other", b"two")), ValueError, "Windows path collision"),
            (zip_data((link, b"target")), ValueError, "symlink or nonregular ZIP entry"),
            (ordinary[:-4], zipfile.BadZipFile, "File is not a zip file"),
            (directory.replace(b"src/", b"../x", 1), zipfile.BadZipFile, "differ"),
            (ordinary + b"PFS0", ValueError, "ZIP has trailing data"),
            (b"PFS0" + ordinary, ValueError, "game material signature"),
            (b"prefix" + ordinary, ValueError, "ZIP has leading data"),
        )
        for contents, exception, reason in cases:
            with self.subTest(reason=reason), self.assertRaisesRegex(exception, reason):
                validate_entries((PYTHON_ARCHIVE, contents))

    def test_tar_members_and_boundaries(self):
        source = tar_data("unicorn-2.1.4/src/test.c", b"int dummy;")
        tar = gzip.decompress(source)
        cases = (
            (tar_data("src/private.bin", b"BNTXpayload"), ValueError, "game material signature"),
            (tar_data("src/../private", b"dummy"), ValueError, "unsafe Windows path component"),
            (tar_data("src/link", b"", link=True), ValueError, "link, device or nonregular TAR entry"),
            (source[:-8], EOFError, "Compressed file ended"),
            (gzip.compress(tar[:1024]), ValueError, "missing or incomplete end blocks"),
            (gzip.compress(tar[:600] + b"PFS0" + tar[604:]), ValueError, "invalid TAR padding"),
        )
        for contents, exception, reason in cases:
            with self.subTest(reason=reason), self.assertRaisesRegex(exception, reason):
                validate_entries((UNICORN_ARCHIVE, contents))

    def test_unexpected_outer_archive(self):
        ordinary = zip_data(("pkg/stdlib.pyc", b"python fixture"))
        for extension in ("zip", "bin"):
            with self.subTest(extension=extension), self.assertRaisesRegex(ValueError, "unexpected archive"):
                validate_entries((f"EoWRecomp/eowgen/_internal/extra.{extension}", ordinary))


if __name__ == "__main__":
    unittest.main()
