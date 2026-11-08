"""Exercise the real ExternalProject integration using a tiny local runtime."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake and Ninja required")
class OnnxRuntimeBuildTests(unittest.TestCase):
    def test_unchanged_build_is_idle_and_source_edits_rebuild(self):
        with tempfile.TemporaryDirectory(prefix="brockdj-ort-build-") as directory:
            root = Path(directory)
            source = root / "libs/onnxruntime-src"
            (source / "cmake").mkdir(parents=True)
            (source / "include/onnxruntime/core/session").mkdir(parents=True)
            (source / "onnxruntime").mkdir()
            (source / "VERSION_NUMBER").write_text("1.0.0\n")
            runtime = source / "onnxruntime/runtime.cpp"
            runtime.write_text('extern "C" int value() { return 0; }\n')
            (source / "cmake/CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.28)
project(FakeRuntime LANGUAGES CXX)
file(GLOB runtime_sources CONFIGURE_DEPENDS ../onnxruntime/*.cpp)
add_library(onnxruntime SHARED ${runtime_sources})
set_target_properties(onnxruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
install(TARGETS onnxruntime LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
''')
            (root / "main.cpp").write_text('extern "C" int value(); int main() { return value(); }\n')
            (root / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.28)
project(Integration LANGUAGES C CXX)
function(_rdbj_define_imported_binary_target name include_dir library)
    add_library(${name} UNKNOWN IMPORTED)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION "${library}"
        INTERFACE_INCLUDE_DIRECTORIES "${include_dir}")
endfunction()
include("''' + (REPO / "cmake/OnnxRuntime.cmake").as_posix() + '''")
add_executable(app main.cpp)
target_link_libraries(app PRIVATE ThirdPartyOnnxRuntime)
set_target_properties(app PROPERTIES BUILD_RPATH "${_ort_lib_dir}")
if(WIN32)
    add_custom_command(TARGET app POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_RUNTIME_DLL}" "$<TARGET_FILE_DIR:app>")
endif()
''')
            build = root / "build"
            env = dict(os.environ, CCACHE_DISABLE="1")

            def run(*args):
                result = subprocess.run(args, cwd=root, env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout)
                return result.stdout

            run("cmake", "-S", str(root), "-B", str(build), "-G", "Ninja")
            command = ("cmake", "--build", str(build), "--target", "app")
            run(*command)
            app = build / ("app.exe" if os.name == "nt" else "app")
            run(str(app))
            before = app.stat().st_mtime_ns
            idle = run(*command)
            self.assertIn("no work to do", idle)
            self.assertEqual(app.stat().st_mtime_ns, before)

            runtime.write_text('extern "C" int value() { return 3; }\n')
            run(*command)
            self.assertEqual(subprocess.run([str(app)], env=env, check=False).returncode, 3)
            self.assertIn("no work to do", run(*command))

            # Added/deleted files must invalidate the outer step too, so the
            # inner CMake project gets a chance to update its source list.
            extra = source / "onnxruntime/extra.cpp"
            for add_file in (True, False):
                before = app.stat().st_mtime_ns
                if add_file:
                    extra.write_text('extern "C" int extra() { return 7; }\n')
                else:
                    extra.unlink()
                run(*command)
                self.assertNotEqual(app.stat().st_mtime_ns, before)
                self.assertEqual(subprocess.run([str(app)], env=env, check=False).returncode, 3)
                self.assertIn("no work to do", run(*command))
