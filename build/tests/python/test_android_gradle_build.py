# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import, print_function, unicode_literals

import hashlib
import os
import subprocess
from collections import defaultdict
from copy import deepcopy
from pathlib import Path

import mozunit
import pytest
from mach.util import get_state_dir
from mozpack.files import JarFinder
from mozpack.mozjar import JarReader


@pytest.fixture(scope="session")
def topsrcdir():
    here = Path(__file__).parent
    return here.parent.parent.parent


@pytest.fixture(scope="session")
def objdir(topsrcdir):
    return (
        Path(get_state_dir(specific_to_topsrcdir=True, topsrcdir=str(topsrcdir)))
        / "android-gradle-build"
        / "objdir"
    )


@pytest.fixture(scope="session")
def mozconfig(objdir, tmp_path_factory):
    p = tmp_path_factory.mktemp("build") / "mozconfig"
    # p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(
        f"""
ac_add_options --enable-application=mobile/android
ac_add_options --enable-artifact-builds
ac_add_options --target=arm
mk_add_options MOZ_OBJDIR="{objdir}"
"""
    )
    return p


@pytest.fixture(scope="session")
def run_mach(mozconfig):
    import mach_initialize
    from mach.config import ConfigSettings
    from mozbuild.base import MozbuildObject

    here = os.path.abspath(os.path.dirname(__file__))
    os.environ["MOZCONFIG"] = str(mozconfig)
    os.environ["MACH_NO_TERMINAL_FOOTER"] = "1"
    os.environ["MACH_NO_WRITE_TIMES"] = "1"
    build = MozbuildObject.from_environment(cwd=here)

    mach = mach_initialize.initialize(build.topsrcdir)

    def inner(argv):
        mach.settings = ConfigSettings()
        try:
            result = mach.run(argv)  # , stdout=stdout, stderr=stderr)
        except SystemExit:
            result = None

        stdout, stderr = None, None  # capfd.readouterr()
        return (result, stdout, stderr)

    # return (result, stdout.getvalue(), stderr.getvalue())

    return inner


AARS = {
    "geckoview.aar": "gradle/build/mobile/android/geckoview/outputs/aar/geckoview-withGeckoBinaries-debug.aar",
}


APKS = {
    "test_runner.apk": "gradle/build/mobile/android/test_runner/outputs/apk/withGeckoBinaries/debug/test_runner-withGeckoBinaries-debug.apk",
    "androidTest": "gradle/build/mobile/android/geckoview/outputs/apk/androidTest/withGeckoBinaries/debug/geckoview-withGeckoBinaries-debug-androidTest.apk",
    "geckoview_example.apk": "gradle/build/mobile/android/geckoview_example/outputs/apk/withGeckoBinaries/debug/geckoview_example-withGeckoBinaries-debug.apk",
    "messaging_example.apk": "gradle/build/mobile/android/examples/messaging_example/outputs/apk/withGeckoBinaries/debug/messaging_example-withGeckoBinaries-debug.apk",
    "port_messaging_example.apk": "gradle/build/mobile/android/examples/port_messaging_example/outputs/apk/withGeckoBinaries/debug/port_messaging_example-withGeckoBinaries-debug.apk",
}


def hashes(objdir, pattern, targets={**AARS, **APKS}):
    target_to_hash = {}
    hash_to_target = defaultdict(list)
    for shortname, target in targets.items():
        finder = JarFinder(target, JarReader(str(objdir / target)))
        hasher = hashlib.blake2b()

        # We sort paths.  This allows a pattern like `classes*.dex` to capture
        # changes to any of the DEX files, no matter how they are ordered in an
        # AAR or APK.
        for p, f in sorted(finder.find(pattern), key=lambda x: x[0]):
            fp = f.open()
            while True:
                data = fp.read(8192)
                if not len(data):
                    break
                hasher.update(data)

        h = hasher.hexdigest()
        target_to_hash[shortname] = h
        hash_to_target[h].append(shortname)

    return target_to_hash, hash_to_target


def lib_hashes(objdir):
    # We'll take libmozglue.so and libxul.so as representatives for
    # all of the libraries.
    lib_hash_from = {"stripped": {}, "unstripped": {}}
    lib_hash_to = {"stripped": {}, "unstripped": {}}
    # lib_hash_orig = {'stripped': {}, 'unstripped': {}}

    # The libs are stripped in APKs but not in AARs.
    for stripped, targets in (("stripped", APKS), ("unstripped", AARS)):
        for lib in ("libmozglue.so", "libxul.so"):
            lib_hash_from[stripped][lib], lib_hash_to[stripped][lib] = hashes(
                objdir, f"lib/**/{lib}", targets=targets
            )
            # Each lib should be in each target, but it's possible not all targets have the latest lib.
            assert (
                len(lib_hash_to[stripped][lib]) >= 1
            ), f"{lib} is present in {stripped} targets"
            # lib_hash_orig[stripped][lib], = lib_hash_to[stripped][lib].values()

    # return lib_hash_from, lib_hash_to, lib_hash_orig
    return lib_hash_from, lib_hash_to


def test_1(topsrcdir, objdir, mozconfig, run_mach, capfd):
    # assert str(sys.executable) == ""

    # assert (str(topsrcdir), str(objdir), str(mozconfig)) == ""
    # assert str(mozconfig) == ""
    # assert str(Path(objdir) / "target") == ""

    # here = os.path.abspath(os.path.dirname(__file__))
    # build = MozbuildObject.from_environment(cwd=here)

    env = os.environ.copy()
    env["MOZCONFIG"] = str(mozconfig)
    env["MACH_NO_TERMINAL_FOOTER"] = "1"
    env["MACH_NO_WRITE_TIMES"] = "1"
    proc = subprocess.run(
        [str(topsrcdir / "mach"), "build"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        cwd=str(topsrcdir),
    )

    # Always log output.
    # print(f"len(proc.stdout)={len(proc.stdout)}")
    # print(">>>")
    # print(proc.stdout)
    # print("<<<")

    # ret, stdout, stderr = run_mach(["build"])
    # out = capfd.readouterr()
    # stdout = out.out
    # stderr = out.err

    # # print(f"len(stderr)={len(stderr)}")
    # # print(">>>")
    # # print(stderr)
    # # print("<<<")

    assert "Task :machBuildFaster SKIPPED" in proc.stdout
    assert "Skipping task :machBuildFaster because: within `mach build`" in proc.stdout
    assert "Task :machStagePackage SKIPPED" in proc.stdout
    assert "Skipping task :machStagePackage because: within `mach build`" in proc.stdout
    assert proc.returncode == 0

    # Order matters, since `mach build stage-package` depends on the
    # outputs of `mach build faster`.
    assert proc.stdout.find("Task :machBuildFaster SKIPPED") < proc.stdout.find(
        "Task :machStagePackage SKIPPED"
    )

    _, omnijar_hash_to = hashes(objdir, "assets/omni.ja")
    assert len(omnijar_hash_to) == 1
    (omnijar_hash_orig,) = omnijar_hash_to.values()

    lib_hashes(objdir)
    # XXX

    # Now build via Gradle or equivalently via Android Studio.
    proc = subprocess.run(
        [
            str(topsrcdir / "mach"),
            "gradle",
            "geckoview_example:assembleWithGeckoBinariesDebug",
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        cwd=str(topsrcdir),
    )

    assert proc.returncode == 0
    assert "Executing task :machBuildFaster" in proc.stdout
    assert "Executing task :machStagePackage" in proc.stdout

    # Order matters, since `mach build stage-package` depends on the
    # outputs of `mach build faster`.
    assert proc.stdout.find("Executing task :machBuildFaster") < proc.stdout.find(
        "Executing task :machStagePackage"
    )

    _, omnijar_hash_to = hashes(objdir, "assets/omni.ja")
    assert len(omnijar_hash_to) == 1
    (omnijar_hash_new,) = omnijar_hash_to.values()

    assert omnijar_hash_orig == omnijar_hash_new

    # Bug 1627796 exposed an issue where, in substituted builds, changing one
    # .so library required changing every .so library.  Verify that this does
    # not happen with newer Android-Gradle plugin versions.

    def print_mozglue_shasums():
        proc = subprocess.run(
            ["shasum", f'{objdir/"dist/bin/libmozglue.so"}'],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            cwd=str(topsrcdir),
        )
        assert proc.returncode == 0

        print(f"len(proc.stdout)={len(proc.stdout)}")
        print(">>>")
        print(proc.stdout)
        print("<<<")

        proc = subprocess.run(
            ["shasum", f'{objdir/"dist/geckoview/lib/armeabi-v7a/libmozglue.so"}'],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            cwd=str(topsrcdir),
        )
        assert proc.returncode == 0

        print(f"len(proc.stdout)={len(proc.stdout)}")
        print(">>>")
        print(proc.stdout)
        print("<<<")

    print_mozglue_shasums()

    # Publish binaries for consumption via substitution.
    proc = subprocess.run(
        [
            str(topsrcdir / "mach"),
            "gradle",
            "geckoview:publishWithGeckoBinariesDebugPublicationToMavenRepository",
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        cwd=str(topsrcdir),
    )
    assert proc.returncode == 0

    print(f"len(proc.stdout)={len(proc.stdout)}")
    print(">>>")
    print(proc.stdout)
    print("<<<")

    # TODO: check published AAR hashes?

    # Initial build with substitution via Gradle.
    proc = subprocess.run(
        [
            str(topsrcdir / "mach"),
            "gradle",
            "geckoview_example:assembleWithGeckoBinariesDebug",
            "-Ptest_substitute_local_geckoview",
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        cwd=str(topsrcdir),
    )
    assert proc.returncode == 0

    print(f"len(proc.stdout)={len(proc.stdout)}")
    print(">>>")
    print(proc.stdout)
    print("<<<")

    print_mozglue_shasums()

    before = lib_hashes(objdir)

    # Now touch only one library.  This won't process the libraries via `mach
    # artifact install` in the way that `mach build` would, so our changes
    # should be witnessed by Gradle.

    # TODO: context manager.
    with open(objdir / "dist/bin/libmozglue.so", "rb+") as f:
        try:
            f.seek(0)
            before_bytes = f.read(1)
            f.seek(0)
            f.write(b"\0" if before_bytes[0] else b"\1")
            f.flush()

            # Publish updated binaries.
            proc = subprocess.run(
                [
                    str(topsrcdir / "mach"),
                    "gradle",
                    "geckoview:publishWithGeckoBinariesDebugPublicationToMavenRepository",
                ],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                cwd=str(topsrcdir),
            )
            assert proc.returncode == 0

            print(f"len(proc.stdout)={len(proc.stdout)}")
            print(">>>")
            print(proc.stdout)
            print("<<<")

            print_mozglue_shasums()

            # Subsequent build with substitution via Gradle.
            proc = subprocess.run(
                [
                    str(topsrcdir / "mach"),
                    "gradle",
                    "geckoview_example:assembleWithGeckoBinariesDebug",
                    "-Ptest_substitute_local_geckoview",
                ],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                cwd=str(topsrcdir),
            )
            assert proc.returncode == 0

            print(f"len(proc.stdout)={len(proc.stdout)}")
            print(">>>")
            print(proc.stdout)
            print("<<<")

            print_mozglue_shasums()

            after = lib_hashes(objdir)

            # We shouldn't have the same hashes.
            assert before != after

            def assert_changed(before, after, stripped, lib, shortnames):
                before_copy = deepcopy(before)
                after_copy = deepcopy(after)
                before_libs = set()
                after_libs = set()

                # assert before_copy == after_copy
                # assert before_copy != after_copy

                print(stripped, lib, shortnames)

                for shortname in shortnames:
                    print(before_copy[stripped])
                    print(before_copy[stripped][lib])
                    print(before_copy[stripped][lib][shortname])

                    before_libs.add(before_copy[stripped][lib].pop(shortname))
                    after_libs.add(after_copy[stripped][lib].pop(shortname))

                # Before, everything listed was identical.
                assert (
                    len(before_libs) == 1
                ), f"Expected {stripped} {lib} to be the same before in {shortnames}"
                # After, everything listed is identical.
                assert (
                    len(after_libs) == 1
                ), f"Expected {stripped} {lib} to be the same after in {shortnames}"

                # But before and after are different.
                (before_lib,) = list(before_libs)
                (after_lib,) = list(after_libs)

                assert (
                    before_lib != after_lib
                ), f"Expected {stripped} {lib} to have differed in {shortnames}"

                # Everything else is unchanged.
                assert (
                    before_copy == after_copy
                ), f"Expected libraries to be the same before and after other than {shortnames}"

            assert_changed(
                before[0],
                after[0],
                "stripped",
                "libmozglue.so",
                ["geckoview_example.apk"],
            )

        finally:
            f.seek(0)
            f.write(before_bytes)
            f.flush()


if __name__ == "__main__":
    mozunit.main()
