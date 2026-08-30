from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class ImvideoConan(ConanFile):
    name = "imvideo"
    version = "0.1.0"
    package_type = "library"
    license = "Apache-2.0"
    description = "A lightweight Dear ImGui video playback library"
    topics = ("video", "imgui", "ffmpeg", "opengl")
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {
        "shared": False,
        "fPIC": True,
        "ffmpeg/*:with_bzip2": False,
        "ffmpeg/*:with_lzma": False,
        "ffmpeg/*:with_libiconv": False,
        "ffmpeg/*:with_freetype": False,
        "ffmpeg/*:with_fontconfig": False,
        "ffmpeg/*:with_fribidi": False,
        "ffmpeg/*:with_harfbuzz": False,
        "ffmpeg/*:with_libxml2": False,
        "ffmpeg/*:with_openjpeg": False,
        "ffmpeg/*:with_openh264": False,
        "ffmpeg/*:with_opus": False,
        "ffmpeg/*:with_vorbis": False,
        "ffmpeg/*:with_libx264": False,
        "ffmpeg/*:with_libx265": False,
        "ffmpeg/*:with_libvpx": False,
        "ffmpeg/*:with_libmp3lame": False,
        "ffmpeg/*:with_libfdk_aac": False,
        "ffmpeg/*:with_libwebp": False,
        "ffmpeg/*:with_libsvtav1": False,
        "ffmpeg/*:with_libaom": False,
        "ffmpeg/*:with_libdav1d": False,
        "ffmpeg/*:with_soxr": False,
        "ffmpeg/*:disable_all_encoders": True,
    }
    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*", "tests/*", "LICENSE"

    def requirements(self):
        self.requires("ffmpeg/7.1.5")

    def build_requirements(self):
        skip_tests = self.conf.get("tools.build:skip_test", default=False, check_type=bool)
        if not skip_tests and can_run(self):
            self.test_requires("catch2/3.8.1")

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        self.options["ffmpeg"].avdevice = False
        self.options["ffmpeg"].avfilter = True
        self.options["ffmpeg"].postproc = False
        self.options["ffmpeg"].with_programs = False
        if self.settings.os == "Linux":
            self.options["ffmpeg"].with_libalsa = False
            self.options["ffmpeg"].with_pulse = False
            self.options["ffmpeg"].with_xcb = False
            self.options["ffmpeg"].with_xlib = False
        elif self.settings.os == "Macos":
            self.options["ffmpeg"].with_appkit = False
            self.options["ffmpeg"].with_avfoundation = False
        elif self.settings.os == "Windows":
            self.options["ffmpeg"].with_ssl = False

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        toolchain = CMakeToolchain(self, generator="Ninja")
        skip_tests = self.conf.get("tools.build:skip_test", default=False, check_type=bool)
        toolchain.variables["IMVIDEO_BUILD_TESTS"] = not skip_tests and can_run(self)
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        skip_tests = self.conf.get("tools.build:skip_test", default=False, check_type=bool)
        if not skip_tests and can_run(self):
            cmake.ctest(cli_args=["--output-on-failure"])

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        CMake(self).install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "imvideo")
        self.cpp_info.set_property("cmake_target_name", "imvideo::imvideo")
        self.cpp_info.libs = ["imvideo"]
        if self.settings.os == "Macos":
            self.cpp_info.frameworks = ["OpenGL"]
        elif self.settings.os == "Linux":
            self.cpp_info.system_libs = ["GL"]
