"""
patch_libnet80211.py — Pre-build: weaken ieee80211_raw_frame_sanity_check.

Background
----------
Espressif's closed-source libnet80211.a contains a strong symbol
``ieee80211_raw_frame_sanity_check`` that blocks raw 802.11 management
frame injection (deauth, beacon, etc.) through esp_wifi_80211_tx().

This script uses objcopy --weaken-symbol to demote that symbol from
strong → weak.  The linker then prefers the user-supplied override in
arch/esp32/arduino/wsl_bypasser.c, which returns 0 (pass) unconditionally,
allowing all frame types to be injected.

This technique is known as "wsl_bypasser" in the ESP32 community.

Idempotency
-----------
A sentinel file  <lib>.wsl_patched  is created next to libnet80211.a
after a successful patch.  Subsequent builds skip the objcopy call.
The sentinel lives in the PlatformIO package cache — not in the repo —
so it persists across incremental builds but is automatically gone
after a fresh SDK install (which triggers a re-patch).

Supported chips
---------------
  esp32, esp32s2, esp32s3  (Xtensa toolchain)

To add esp32c3/h2 (RISC-V): extend CHIP_MAP with
  "esp32c3": ("esp32c3", "riscv32-esp-elf-objcopy")
and ensure the matching toolchain package is installed.
"""
Import("env")  # noqa: F821  (PlatformIO SCons injection)

import os
import shutil
import subprocess

# ── Config ────────────────────────────────────────────────────────────────

SYMBOL = "ieee80211_raw_frame_sanity_check"

# chip id → (sdk subdir inside tools/sdk/, objcopy binary name)
CHIP_MAP = {
    "esp32":   ("esp32",   "xtensa-esp32-elf-objcopy"),
    "esp32s2": ("esp32s2", "xtensa-esp32s2-elf-objcopy"),
    "esp32s3": ("esp32s3", "xtensa-esp32s3-elf-objcopy"),
}

# ── Helpers ───────────────────────────────────────────────────────────────

def _find_objcopy(name):
    """Search PlatformIO toolchain packages, then fall back to PATH."""
    packages = os.path.join(os.path.expanduser("~"), ".platformio", "packages")
    if os.path.isdir(packages):
        for pkg in os.listdir(packages):
            candidate = os.path.join(packages, pkg, "bin", name)
            if os.path.isfile(candidate):
                return candidate
    return shutil.which(name)

# ── Main ──────────────────────────────────────────────────────────────────

platform = env.subst("$PIOPLATFORM")  # noqa: F821

if platform != "espressif32":
    # native / other — nothing to do
    pass
else:
    mcu = env.subst("$BOARD_MCU").lower()  # noqa: F821  e.g. "esp32s3"

    if mcu not in CHIP_MAP:
        print(f"[wsl_bypass] MCU '{mcu}' not in CHIP_MAP — skipping")
    else:
        sdk_subdir, objcopy_name = CHIP_MAP[mcu]

        # $FRAMEWORK_DIR may be empty in pre-scripts; derive the path
        # directly from the PlatformIO packages directory.
        pio_home = os.environ.get("PLATFORMIO_HOME_DIR",
                                  os.path.join(os.path.expanduser("~"),
                                               ".platformio"))
        pkgs = os.path.join(pio_home, "packages")
        # Arduino-ESP32 v3+ splits libs into a separate package:
        #   framework-arduinoespressif32-libs/{chip}/lib/libnet80211.a
        # Older releases kept them inside the framework package:
        #   framework-arduinoespressif32/tools/sdk/{chip}/lib/libnet80211.a
        candidate_paths = [
            os.path.join(pkgs, "framework-arduinoespressif32-libs",
                         sdk_subdir, "lib", "libnet80211.a"),
            os.path.join(pkgs, "framework-arduinoespressif32",
                         "tools", "sdk", sdk_subdir, "lib", "libnet80211.a"),
        ]
        lib_path = next((p for p in candidate_paths if os.path.isfile(p)), None)
        sentinel  = (lib_path + ".wsl_patched") if lib_path else None

        if lib_path is None:
            tried = "\n  ".join(candidate_paths)
            print(f"[wsl_bypass] libnet80211.a not found, tried:\n  {tried}")
        elif os.path.isfile(sentinel):
            print(f"[wsl_bypass] libnet80211.a ({mcu}) already patched — ok")
        else:
            objcopy = _find_objcopy(objcopy_name)
            if not objcopy:
                print(f"[wsl_bypass] {objcopy_name} not found — cannot patch")
            else:
                tmp = lib_path + ".wsl_tmp"
                try:
                    subprocess.check_call(
                        [objcopy,
                         f"--weaken-symbol={SYMBOL}",
                         lib_path, tmp],
                    )
                    shutil.move(tmp, lib_path)
                    open(sentinel, "w").close()
                    print(f"[wsl_bypass] Patched libnet80211.a ({mcu}): "
                          f"{SYMBOL} is now weak")
                except subprocess.CalledProcessError as exc:
                    print(f"[wsl_bypass] objcopy failed: {exc}")
                    if os.path.isfile(tmp):
                        os.remove(tmp)
