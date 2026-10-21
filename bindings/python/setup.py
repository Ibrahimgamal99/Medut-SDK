from setuptools import setup, Distribution
try:
    from setuptools.command.bdist_wheel import bdist_wheel  # setuptools >= 70.1
except ImportError:
    from wheel.bdist_wheel import bdist_wheel

# Whichever core is in medut/ goes in the wheel; build.sh / build.ps1 and
# .github/workflows/pypi.yml copy exactly one there. Listing all three
# lets one host build every platform's wheel.
native_lib = ["medut.so", "medut.dll", "medut.dylib"]


class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True  # forces setuptools to use platlib, not purelib


class BinaryWheel(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        _, _, plat = super().get_tag()
        return "py3", "none", plat  # any Python 3, no ABI, native platform


setup(
    distclass=BinaryDistribution,
    cmdclass={"bdist_wheel": BinaryWheel},
    package_data={"medut": native_lib + ["_medut_clean.h"]},
)
