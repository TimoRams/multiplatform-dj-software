import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


REPO = Path(__file__).resolve().parents[2]


class PackagingScriptsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="brockdj-packaging-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.bin = self.root / "tools"
        self.bin.mkdir()
        self.log = self.root / "deployment.json"
        self.qt = self.root / "qt"
        plugins = self.qt / "plugins"
        for category, names in {
            "platforms": ["libqxcb.so", "libqoffscreen.so", "libqoffscreen.dylib"],
            "sqldrivers": ["libqsqlite.so", "libqsqlite.dylib"],
        }.items():
            (plugins / category).mkdir(parents=True)
            for name in names:
                (plugins / category / name).touch()
        (self.qt / "lib").mkdir()
        self.tool(self.qt / "bin/qmake", f"""
            import sys
            print({{
                "QT_VERSION": "6.8.3",
                "QT_INSTALL_PREFIX": {str(self.qt)!r},
                "QT_INSTALL_PLUGINS": {str(plugins)!r},
                "QT_INSTALL_LIBS": {str(self.qt / "lib")!r},
            }}[sys.argv[-1]])
        """)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                        QT_ROOT_DIR=str(self.qt), RUNNER_TEMP=str(self.root))
        scripts = self.root / "scripts/ci"
        scripts.mkdir(parents=True)
        for name in ("package-appimage.sh", "package-macos.sh"):
            shutil.copyfile(REPO / "scripts/ci" / name, scripts / name)

    def tool(self, path, body):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/usr/bin/env python3\n" + textwrap.dedent(body))
        path.chmod(0o755)

    def run_script(self, name, *args):
        return subprocess.run(
            ["bash", str(self.root / "scripts/ci" / name), *map(str, args)],
            env=self.env, capture_output=True, text=True, timeout=15,
        )

    def prepare_linux(self, soname="libsqlcipher.so.1", deploy=False):
        self.tool(self.bin / "uname", 'print("x86_64")')
        self.tool(self.bin / "ldd", 'print("statically linked")')
        binary = self.root / "BrockDJ"
        shutil.copyfile(shutil.which("true"), binary)
        binary.chmod(0o755)
        library = self.root / "libsqlcipher.so"
        subprocess.run(
            ["cc", "-shared", "-fPIC", f"-Wl,-soname,{soname}",
             "-x", "c", "-", "-o", str(library)],
            input="int fixture(void) { return 0; }\n", text=True, check=True,
            capture_output=True,
        )
        self.metadata = Path(f"{binary}.sqlcipher-runtime")
        self.metadata.write_text(f"{library}\n{soname}\n")
        icon = self.root / "resources/icons/generated/256.png"
        icon.parent.mkdir(parents=True, exist_ok=True)
        icon.touch()
        tools = self.root / "brockdj-appimage-tools-x86_64"
        for name in ("linuxdeploy-plugin-qt.AppImage",
                     "linuxdeploy-plugin-appimage.AppImage", "runtime-x86_64"):
            self.tool(tools / name, "raise SystemExit(77)")
        self.tool(tools / "linuxdeploy.AppImage", f"""
            import json, pathlib, shutil, sys
            args = sys.argv[1:]
            pathlib.Path({str(self.log)!r}).write_text(json.dumps(args))
            if not {deploy!r}:
                raise SystemExit(77)
            libdir = pathlib.Path(args[args.index("--appdir") + 1]) / "usr/lib"
            libdir.mkdir(parents=True)
            source = pathlib.Path(args[args.index("--library") + 1])
            shutil.copyfile(source, libdir / source.name)
            shutil.copyfile(source, libdir / "libqsqlite.so")
        """)
        return binary, library

    def test_linux_explicitly_deploys_the_configured_runtime(self):
        for soname in ("libsqlcipher.so.1", "libsqlcipher-3.39.2.so.0"):
            with self.subTest(soname=soname):
                binary, library = self.prepare_linux(soname, deploy=True)
                result = self.run_script("package-appimage.sh", binary, self.root / "out.AppImage")
                self.assertIn("libkeyfinder runtime is missing", result.stderr)
                self.assertNotEqual(result.returncode, 0)
                args = json.loads(self.log.read_text())
                self.assertEqual(args[args.index("--library") + 1], str(library))
                deployed = self.root / "BrockDJ-x86_64.AppDir/usr/lib" / soname
                self.assertTrue(deployed.is_file())
                self.assertEqual(deployed.read_bytes(), library.read_bytes())

    def test_linux_rejects_missing_incomplete_or_wrong_abi_metadata(self):
        binary, library = self.prepare_linux()
        for contents, error in (
            (None, "deployment metadata is missing"),
            (f"{library}\n", "Incomplete SQLCipher deployment metadata"),
            (f"{library}\n../libsqlcipher.so.1\n", "Invalid SQLCipher deployment metadata"),
            (f"{library}.missing\nlibsqlcipher.so.1\n", "Invalid SQLCipher deployment metadata"),
            (f"{library}\nlibsqlcipher.so.999\n", "runtime ABI differs"),
        ):
            with self.subTest(error=error):
                if contents is None:
                    self.metadata.unlink()
                else:
                    self.metadata.write_text(contents)
                result = self.run_script("package-appimage.sh", binary, self.root / "out.AppImage")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertFalse(self.log.exists())

    def test_linux_rejects_direct_sqlcipher_linkage(self):
        binary, _ = self.prepare_linux()
        self.tool(self.bin / "ldd", 'print("libsqlcipher.so.1 => /lib/libsqlcipher.so.1")')
        result = self.run_script("package-appimage.sh", binary, self.root / "out.AppImage")
        self.assertIn("must load SQLCipher privately", result.stderr)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.log.exists())

    def test_linux_reports_failed_or_unresolved_dependency_inspection(self):
        binary, _ = self.prepare_linux()
        for body, error in (
            ('print("ldd failed"); raise SystemExit(1)', "Could not inspect BrockDJ"),
            ('print("libmissing.so.1 => not found")', "unresolved shared-library dependencies"),
        ):
            with self.subTest(error=error):
                self.tool(self.bin / "ldd", body)
                result = self.run_script("package-appimage.sh", binary, self.root / "out.AppImage")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertFalse(self.log.exists())

    def test_macos_supplies_built_runtime_search_path_and_aborts_interactive_lookup(self):
        # An SDK left by an older checkout must never enter dependency lookup.
        (self.root / "libs/onnxruntime/lib").mkdir(parents=True)
        source_lib = self.root / "build/onnxruntime/lib"
        source_lib.mkdir(parents=True)
        app = self.root / "build/bin/BrockDJ.app"
        binary = app / "Contents/MacOS/BrockDJ"
        self.tool(binary, "raise SystemExit(0)")
        self.tool(self.qt / "bin/macdeployqt", "raise SystemExit(0)")
        self.tool(self.bin / "dylibbundler", f"""
            import json, pathlib, sys
            pathlib.Path({str(self.log)!r}).write_text(
                json.dumps({{"args": sys.argv[1:], "stdin": sys.stdin.read()}}))
            raise SystemExit(77)
        """)
        result = self.run_script("package-macos.sh", app, "arm64", self.root / "out.zip")
        self.assertEqual(result.returncode, 77, result.stderr)
        invocation = json.loads(self.log.read_text())
        args = invocation["args"]
        search_paths = [args[i + 1] for i, arg in enumerate(args) if arg == "-s"]
        self.assertEqual(search_paths, [
            str(self.qt / "lib"), str(app / "Contents/Frameworks"), str(source_lib),
        ])
        self.assertEqual(invocation["stdin"], "quit\n")


if __name__ == "__main__":
    unittest.main()
