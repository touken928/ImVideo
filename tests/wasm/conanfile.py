from pathlib import Path

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain


class WasmPackageTest(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    test_type = "explicit"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def validate(self):
        if self.settings.os != "Emscripten":
            raise ConanInvalidConfiguration("This consumer tests the Emscripten package")

    def layout(self):
        build = Path(self.recipe_folder).parents[1] / "build" / "wasm-tests"
        self.folders.build = str(build)
        self.folders.generators = str(build / "generators")

    def generate(self):
        deps = CMakeDeps(self)
        deps.set_property("imvideo", "cmake_find_mode", "none")
        deps.generate()
        toolchain = CMakeToolchain(self, generator="Ninja")
        # Test installed CMake exports, not just the Conan-generated wrapper.
        toolchain.variables["IMVIDEO_PACKAGE_DIR"] = str(Path(self.dependencies["imvideo"].package_folder))
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        self.output.info("Run tests/wasm/run.py against the build folder for browser execution")
