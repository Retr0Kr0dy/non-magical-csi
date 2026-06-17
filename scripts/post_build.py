"""
post_build.py - non-magical-csi post-build artifact copy.

Copies the factory-flash binary to:
  bin/non-magical-csi-<version>-<env>.bin

<version> : value of CSI_SENSE_VERSION build flag
<env>     : PlatformIO environment name (m5-cardputer-adv, m5-cardputer)

Prefers firmware.factory.bin (full merged image, flash at 0x0).
Falls back to firmware.bin if factory image was not generated.
"""
Import("env")  # noqa: F821  (PlatformIO SCons injection)

import os
import re
import shutil


def _extract_version(build_flags):
    for flag in build_flags:
        m = re.search(r'CSI_SENSE_VERSION=\\"([^\\"]+)\\"', str(flag))
        if m:
            return m.group(1)
    return "0.0.0"


def copy_artifact(source, target, env):  # noqa: ARG001
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    pioenv      = env.subst("$PIOENV")

    version = _extract_version(env.get("BUILD_FLAGS", []))

    bin_dir = os.path.join(project_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)

    factory = os.path.join(build_dir, "firmware.factory.bin")
    plain   = os.path.join(build_dir, "firmware.bin")

    src = factory if os.path.isfile(factory) else plain
    if not os.path.isfile(src):
        print(f"\n[non-magical-csi] WARNING: no artifact found in {build_dir}")
        return

    dst = os.path.join(bin_dir, f"non-magical-csi-{version}-{pioenv}.bin")
    shutil.copy2(src, dst)
    rel     = os.path.relpath(dst, project_dir)
    size_kb = os.path.getsize(dst) / 1024
    merged  = "(factory merged)" if src == factory else "(firmware only - flash at 0x10000)"
    print(f"\n  * non-magical-csi artifact  ->  {rel}  ({size_kb:.1f} KB)  {merged}\n")


env.AddPostAction("$BUILD_DIR/firmware.bin", copy_artifact)  # noqa: F821
