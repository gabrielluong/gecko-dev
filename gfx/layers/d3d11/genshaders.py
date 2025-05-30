# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
import argparse
import codecs
import locale
import os
import re
import subprocess
import sys
import tempfile

import buildconfig
import yaml


def shell_main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", type=str, required=True, help="Output file")
    parser.add_argument("manifest", type=str, help="Manifest source file")
    args = parser.parse_args()

    with open(args.output, "w") as out_file:
        process_manifest(out_file, args.manifest)


def main(output_fp, input_filename):
    return process_manifest(output_fp, input_filename)


HEADER = """// AUTOGENERATED - DO NOT EDIT
namespace mozilla {
namespace layers {

struct ShaderBytes { const void* mData; size_t mLength; };
"""
FOOTER = """
} // namespace layers
} // namespace mozilla"""


def process_manifest(output_fp, manifest_filename):
    with codecs.open(manifest_filename, "r", "UTF-8") as in_fp:
        manifest = yaml.safe_load(in_fp)
    shader_folder, _ = os.path.split(manifest_filename)

    output_fp.write(HEADER)

    deps = set()
    for block in manifest:
        if "type" not in block:
            raise Exception("Expected 'type' key with shader mode")
        if "file" not in block:
            raise Exception("Expected 'file' key with shader file")
        if "shaders" not in block:
            raise Exception("Expected 'shaders' key with shader name list")

        shader_file = os.path.join(shader_folder, block["file"])
        deps.add(shader_file)

        shader_model = block["type"]
        for shader_name in block["shaders"]:
            new_deps = run_fxc(
                shader_model=shader_model,
                shader_file=shader_file,
                shader_name=shader_name,
                output_fp=output_fp,
            )
            deps |= new_deps

    output_fp.write(FOOTER)
    return deps


def run_fxc(shader_model, shader_file, shader_name, output_fp):
    fxc_location = buildconfig.substs["FXC"]

    argv = [
        fxc_location,
        "-nologo",
        f"-T{shader_model}",
        os.path.relpath(shader_file),
        f"-E{shader_name}",
        f"-Vn{shader_name}",
        "-Vi",
    ]
    if "WINNT" not in buildconfig.substs["HOST_OS_ARCH"]:
        argv.insert(0, buildconfig.substs["WINE"])
    if shader_model.startswith("vs_"):
        argv += ["-DVERTEX_SHADER"]
    elif shader_model.startswith("ps_"):
        argv += ["-DPIXEL_SHADER"]

    deps = None
    with ScopedTempFilename() as temp_filename:
        argv += [f"-Fh{os.path.relpath(temp_filename)}"]

        sys.stdout.write("{0}\n".format(" ".join(argv)))
        sys.stdout.flush()
        proc_stdout = subprocess.check_output(argv)
        proc_stdout = decode_console_text(sys.stdout, proc_stdout)
        deps = find_dependencies(proc_stdout)
        assert "fxc2" in fxc_location or len(deps) > 0

        with open(temp_filename) as temp_fp:
            output_fp.write(temp_fp.read())

    output_fp.write(
        f"ShaderBytes s{shader_name} = {{ {shader_name}, sizeof({shader_name}) }};\n"
    )
    return deps


def find_dependencies(fxc_output):
    # Dependencies look like this:
    #   Resolved to [<path>]
    #
    # Microsoft likes to change output strings based on the user's language, so
    # instead of pattern matching on that string, we take everything in between
    # brackets. We filter out potentially bogus strings later.
    deps = set()
    for line in fxc_output.split("\n"):
        m = re.search(r"\[([^\]]+)\]", line)
        if m is None:
            continue
        dep_path = m.group(1)
        dep_path = os.path.normpath(dep_path)
        # When run via Wine, FXC's output contains Windows paths on the Z drive.
        # We want to normalize them back to unix paths for the build system.
        if "WINNT" not in buildconfig.substs[
            "HOST_OS_ARCH"
        ] and dep_path.lower().startswith("z:"):
            dep_path = dep_path[2:].replace("\\", "/")
        if os.path.isfile(dep_path):
            deps.add(dep_path)
    return deps


# Python reads the raw bytes from stdout, so we need to try our best to
# capture that as a valid Python string.


def decode_console_text(pipe, text):
    try:
        if pipe.encoding:
            return text.decode(pipe.encoding, "replace")
    except Exception:
        pass
    try:
        return text.decode(locale.getpreferredencoding(), "replace")
    except Exception:
        return text.decode("utf8", "replace")


# Allocate a temporary file name and delete it when done. We need an extra
# wrapper for this since TemporaryNamedFile holds the file open.


class ScopedTempFilename:
    def __init__(self):
        self.name = None

    def __enter__(self):
        with tempfile.NamedTemporaryFile(dir=os.getcwd(), delete=False) as tmp:
            self.name = tmp.name
            return self.name

    def __exit__(self, type, value, traceback):
        if not self.name:
            return
        try:
            os.unlink(self.name)
        except Exception:
            pass


if __name__ == "__main__":
    shell_main()
