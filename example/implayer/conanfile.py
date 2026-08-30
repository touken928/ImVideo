from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class ImplayerConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = ()
    exports_sources = "CMakeLists.txt", "src/*"

    def requirements(self):
        self.requires("imvideo/0.2.0")
        self.requires("glfw/3.4")
        self.requires("imgui/1.92.9b")
        self.requires("miniaudio/0.11.22")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self, generator="Ninja").generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
