import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile


spec = importlib.util.spec_from_file_location(
    "openvino_installer", Path(__file__).resolve().parents[2] / "scripts/fetch-stems-openvino.py"
)
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class OpenVinoInstallerTests(unittest.TestCase):
    def use(self, patcher):
        result = patcher.start()
        self.addCleanup(patcher.stop)
        return result

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.destination = Path(self.temporary.name) / "providers/openvino"
        self.use(patch.object(installer.sys, "platform", "linux"))
        self.use(patch.object(installer.platform, "machine", return_value="x86_64"))

    def archive(self, extra=None, notices=True):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr(
                "onnxruntime_ep_openvino/libonnxruntime_providers_openvino_plugin.so", b"plugin"
            )
            archive.writestr("onnxruntime_ep_openvino/libopenvino.so", b"dependency")
            if notices:
                archive.writestr("onnxruntime_ep_openvino/third-party-programs.txt", b"licenses")
            if extra:
                archive.writestr(extra, b"unexpected")
        return buffer.getvalue()

    def serve(self, data, digest=None):
        self.use(patch.object(installer.urllib.request, "urlopen", return_value=io.BytesIO(data)))
        self.use(patch.dict(installer.ASSETS, {
            "linux": ("https://example.invalid/provider.whl",
                      digest or hashlib.sha256(data).hexdigest(),
                      "libonnxruntime_providers_openvino_plugin.so")
        }))

    def test_installs_libraries_and_notices(self):
        self.serve(self.archive())
        installer.install(self.destination)
        self.assertEqual((self.destination / "libopenvino.so").read_bytes(), b"dependency")
        self.assertEqual((self.destination / "third-party-programs.txt").read_bytes(), b"licenses")
        self.assertEqual(sorted(p.name for p in self.destination.parent.iterdir()), ["openvino"])

    def test_hash_mismatch_leaves_no_installation(self):
        self.serve(self.archive(), "0" * 64)
        with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
            installer.install(self.destination)
        self.assertFalse(self.destination.exists())

    def test_archive_traversal_is_rejected(self):
        self.serve(self.archive("onnxruntime_ep_openvino/../outside"))
        with self.assertRaisesRegex(RuntimeError, "Invalid provider archive path"):
            installer.install(self.destination)
        self.assertFalse(self.destination.exists())
        self.assertFalse((self.destination.parent / "outside").exists())

    def test_missing_notices_prevent_installation(self):
        self.serve(self.archive(notices=False))
        with self.assertRaisesRegex(RuntimeError, "license notices"):
            installer.install(self.destination)
        self.assertFalse(self.destination.exists())

    def test_existing_installation_is_not_overwritten(self):
        self.destination.mkdir(parents=True)
        sentinel = self.destination / "existing"
        sentinel.write_bytes(b"keep")
        with self.assertRaisesRegex(RuntimeError, "Refusing to overwrite"):
            installer.install(self.destination)
        self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_unsupported_architecture_is_explicit(self):
        with patch.object(installer.platform, "machine", return_value="aarch64"):
            with self.assertRaisesRegex(RuntimeError, "Linux or Windows x64"):
                installer.install(self.destination)
        self.assertFalse(self.destination.exists())

    def test_per_user_data_location(self):
        with patch.dict(installer.os.environ, {"XDG_DATA_HOME": self.temporary.name}):
            self.assertEqual(installer.default_destination(),
                             Path(self.temporary.name) / "BrockDJ/stems-providers/openvino")


if __name__ == "__main__":
    unittest.main()
