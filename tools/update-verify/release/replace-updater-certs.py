# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

import os.path
import sys

cert_dir = sys.argv[1]
# Read twice, because strings cannot be copied
updater_data = open(sys.argv[2], "rb").read()
new_updater = open(sys.argv[2], "rb").read()
outfile = sys.argv[3]

cert_pairs = sys.argv[4:]

if (len(cert_pairs) % 2) != 0:
    print("Certs must be provided in pairs")
    sys.exit(1)

for find_cert, replace_cert in zip(*[iter(cert_pairs)] * 2):
    find = open(os.path.join(cert_dir, find_cert), "rb").read()
    replace = open(os.path.join(cert_dir, replace_cert), "rb").read()
    print(f"Looking for {find_cert}...")
    if find in new_updater:
        print(f"Replacing {find_cert} with {replace_cert}")
        new_updater = new_updater.replace(find, replace)
    else:
        print(f"Didn't find {find_cert}...")

if len(updater_data) != len(new_updater):
    print(
        f"WARNING: new updater is not the same length as the old one (old: {len(updater_data)}, new: {len(new_updater)})"
    )

if updater_data == new_updater:
    print("WARNING: updater is unchanged")

with open(outfile, "wb+") as f:
    f.write(new_updater)
