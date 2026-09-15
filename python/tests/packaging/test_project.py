from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python 3.10
    import tomli as tomllib


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


def test_scikit_build_core_is_the_only_build_backend() -> None:
    project = tomllib.loads((REPOSITORY_ROOT / "pyproject.toml").read_text())

    assert project["build-system"]["build-backend"] == "scikit_build_core.build"
    assert project["tool"]["scikit-build"]["wheel"]["packages"] == ["python/mooncake"]
    assert project["tool"]["scikit-build"]["install"]["components"] == ["python"]
    assert project["tool"]["scikit-build"]["sdist"]["include"] == [
        "extern/yalantinglibs/**",
        "!extern/yalantinglibs/.git",
    ]
    assert project["tool"]["scikit-build"]["sdist"]["exclude"] == [
        "extern/yalantinglibs/.git"
    ]
    assert "setuptools>=61" in project["build-system"]["requires"]
    assert "pip>=23" in project["build-system"]["requires"]
    assert project["tool"]["scikit-build"]["cmake"]["define"]["USE_CUDA"] is False
    assert project["tool"]["scikit-build"]["cmake"]["define"]["WITH_EP"] is False


def test_dependency_boundaries_are_declared() -> None:
    project = tomllib.loads((REPOSITORY_ROOT / "pyproject.toml").read_text())
    metadata = project["project"]

    assert set(metadata["dependencies"]) == {"aiohttp", "msgpack", "requests"}
    assert set(metadata["optional-dependencies"]) == {
        "administration",
        "dev",
        "hardware",
        "structured",
        "vllm",
    }


def test_tracked_source_roots_contain_no_generated_native_artifacts() -> None:
    package_root = REPOSITORY_ROOT / "python" / "mooncake"

    assert (package_root / "__init__.py").is_file()
    assert not list(package_root.rglob("*.so"))
    assert not list((REPOSITORY_ROOT / "mooncake-pg" / "torch").rglob("*.so"))


def test_pg_extension_build_stages_outside_the_source_tree(
    tmp_path: Path,
) -> None:
    cmake = shutil.which("cmake")
    if cmake is None:
        pytest.skip("CMake is required to exercise the PG staging script")

    source = tmp_path / "source" / "mooncake-pg" / "torch"
    common = tmp_path / "source" / "mooncake-common"
    source.mkdir(parents=True)
    common.mkdir(parents=True)
    (common / "SetupPyTorchEnv.cmake").write_text("")
    (source / "setup.py").write_text(
        """\
from pathlib import Path
import sys

build_lib = Path(sys.argv[sys.argv.index("--build-lib") + 1])
package = build_lib / "mooncake"
package.mkdir(parents=True, exist_ok=True)
(package / "pg_fake.so").write_bytes(b"extension")
"""
    )

    core = tmp_path / "libmooncake_pg.so"
    device = tmp_path / "libmooncake_pg_device.so"
    core.write_bytes(b"core")
    device.write_bytes(b"device")
    staging = tmp_path / "staging"
    build = tmp_path / "build"

    subprocess.run(
        [
            cmake,
            f"-DSOURCE_DIR={source}",
            "-DEP_TORCH_VERSIONS=",
            f"-DSTAGING_DIR={staging}",
            f"-DBUILD_DIR={build}",
            f"-DPG_CORE_SO_PATH={core}",
            f"-DPG_DEVICE_SO_PATH={device}",
            f"-DPython3_EXECUTABLE={sys.executable}",
            "-P",
            str(REPOSITORY_ROOT / "mooncake-pg" / "torch" / "BuildPgExt.cmake"),
        ],
        check=True,
    )

    assert (staging / "pg_fake.so").read_bytes() == b"extension"
    assert (staging / device.name).read_bytes() == b"device"
    assert (build / "current" / "lib" / "mooncake" / "pg_fake.so").is_file()
    assert not list(source.rglob("*.so"))


@pytest.fixture
def python_install_project(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    if shutil.which("cmake") is None:
        pytest.skip("CMake is required to exercise Python installation")

    source = tmp_path / "source"
    source.mkdir()
    integration = REPOSITORY_ROOT / "mooncake-integration"
    (source / "CMakeLists.txt").write_text(
        f"""\
cmake_minimum_required(VERSION 3.16)
project(PythonInstallContract NONE)
set(Python3_EXECUTABLE "{sys.executable}")
add_subdirectory("{integration}" integration)
get_directory_property(install_dir DIRECTORY "{integration}"
                       DEFINITION MOONCAKE_PYTHON_INSTALL_DIR)
file(WRITE "${{CMAKE_BINARY_DIR}}/install_dir.txt" "${{install_dir}}")
"""
    )
    customization = tmp_path / "customization"
    customization.mkdir()
    (customization / "sitecustomize.py").write_text(
        f"""\
import os
import sys
import sysconfig

sys.path.insert(0, {str(tmp_path / "dsl_packages")!r})
original_get_path = sysconfig.get_path

def get_path(name, *args, **kwargs):
    if name == "platlib":
        mode = os.environ.get("MOONCAKE_TEST_PLATLIB_MODE", "valid")
        if mode == "error":
            raise RuntimeError("platlib probe failed")
        if mode == "empty":
            return ""
        if mode == "relative":
            return "relative-packages"
        return {str(tmp_path / "native-platlib")!r}
    return original_get_path(name, *args, **kwargs)

sysconfig.get_path = get_path
"""
    )
    monkeypatch.setenv("PYTHONPATH", str(customization))
    monkeypatch.delenv("MOONCAKE_TEST_PLATLIB_MODE", raising=False)
    return tmp_path


def _configure_python_install(
    root: Path, *, skbuild: bool = False
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "cmake",
            "-S",
            str(root / "source"),
            "-B",
            str(root / "build"),
            f"-DSKBUILD={'ON' if skbuild else 'OFF'}",
            f"-DCMAKE_INSTALL_PREFIX={root / 'prefix'}",
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def test_python_install_uses_platlib_not_search_path(
    python_install_project: Path,
) -> None:
    root = python_install_project
    result = _configure_python_install(root)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (root / "build/install_dir.txt").read_text() == str(
        root / "native-platlib/mooncake"
    )


@pytest.mark.parametrize("mode", ["error", "empty", "relative"])
def test_python_install_rejects_invalid_platlib(
    python_install_project: Path, monkeypatch: pytest.MonkeyPatch, mode: str
) -> None:
    monkeypatch.setenv("MOONCAKE_TEST_PLATLIB_MODE", mode)
    result = _configure_python_install(python_install_project)
    assert result.returncode != 0
    assert "Python platlib" in result.stdout + result.stderr


def test_skbuild_python_install_stays_relative(python_install_project: Path) -> None:
    root = python_install_project
    result = _configure_python_install(root, skbuild=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (root / "build/install_dir.txt").read_text() == "mooncake"


@pytest.mark.skipif(os.name != "posix", reason="POSIX staging and permission contract")
@pytest.mark.parametrize("existing", [False, True])
def test_python_install_permissions_respect_destdir(
    python_install_project: Path, existing: bool
) -> None:
    root = python_install_project
    result = _configure_python_install(root)
    assert result.returncode == 0, result.stdout + result.stderr

    real_package = root / "native-platlib/mooncake"
    real_package.mkdir(parents=True)
    real_init = real_package / "__init__.py"
    real_init.write_text("# Must not be modified by staged installation\n")
    real_package.chmod(0o755)
    real_init.chmod(0o644)

    stage = root / "stage"
    staged_package = stage / real_package.relative_to(real_package.anchor)
    staged_init = staged_package / "__init__.py"
    if existing:
        staged_package.mkdir(parents=True)
        staged_init.write_text("# Auto-generated by CMake\n")
        staged_package.chmod(0o767)
        staged_init.chmod(0o766)

    result = subprocess.run(
        ["cmake", "--install", str(root / "build")],
        env={**os.environ, "DESTDIR": str(stage)},
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert stat.S_IMODE(staged_package.stat().st_mode) == 0o755
    assert stat.S_IMODE(staged_init.stat().st_mode) == 0o644
    assert (staged_package / "async_store.py").is_file()
    assert not (stage / root.relative_to(root.anchor) / "dsl_packages").exists()
    assert stat.S_IMODE(real_package.stat().st_mode) == 0o755
    assert stat.S_IMODE(real_init.stat().st_mode) == 0o644
    assert real_init.read_text() == "# Must not be modified by staged installation\n"
