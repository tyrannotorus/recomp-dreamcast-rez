#!/usr/bin/env python3
"""Setup, build and run Rez from the repository root.

    python tools/dc.py doctor                  what this machine has / lacks
    python tools/dc.py deps                    fetch SDL3 into .deps/ (Windows: VC devel zip)
    python tools/dc.py setup --disc <image>    verify the disc, extract the boot binary and code
                                               overlays into game/extracted/, remember the image
    python tools/dc.py build [--target T] [--fresh] [--headless]
    python tools/dc.py run [--window] [-- launcher args...]

Builds go to build/. On Windows every compiler call runs inside the Visual Studio developer
environment, found with vswhere. The remembered disc path lives in .rez_recompiled/local.json
(git-ignored).
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, ".deps")
SDL3_VERSION = "3.4.18"
IS_WIN = os.name == "nt"
EXE = ".exe" if IS_WIN else ""


# ---------------------------------------------------------------------------------------------
# environment

def vcvars() -> str | None:
    if not IS_WIN:
        return None
    vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        for line in out.splitlines():
            p = os.path.join(line, r"VC\Auxiliary\Build\vcvars64.bat")
            if os.path.exists(p):
                return p
    return None


_ENV_CACHE: dict | None = None


def build_env() -> dict:
    """os.environ plus the MSVC developer environment on Windows (cached per process)."""
    global _ENV_CACHE
    if _ENV_CACHE is not None:
        return _ENV_CACHE
    env = dict(os.environ)
    vc = vcvars()
    if vc:
        out = subprocess.run(f'cmd /s /c ""{vc}" >nul && set"', capture_output=True, text=True, shell=True).stdout
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
    vk = glob.glob(r"C:\VulkanSDK\*") if IS_WIN else []
    if vk and "VULKAN_SDK" not in env:
        env["VULKAN_SDK"] = sorted(vk)[-1]
    _ENV_CACHE = env
    return env


def run(cmd: list[str], cwd: str | None = None, check: bool = True, env: dict | None = None) -> int:
    print("+ " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd, env=env or build_env())
    if check and r.returncode != 0:
        sys.exit(f"command failed with exit code {r.returncode}")
    return r.returncode


# ---------------------------------------------------------------------------------------------
# project helpers

def port_info() -> dict:
    port = ROOT
    tomls = glob.glob(os.path.join(port, "game", "*.toml"))
    tomls = [t for t in tomls if not t.endswith("suggested.toml")]
    if len(tomls) != 1:
        sys.exit(f"{port}: expected exactly one game/<id>.toml, found {len(tomls)}")
    gid = os.path.splitext(os.path.basename(tomls[0]))[0]
    local_path = os.path.join(port, ".rez_recompiled", "local.json")
    local = json.load(open(local_path)) if os.path.exists(local_path) else {}
    exe_candidates = glob.glob(os.path.join(port, "build", "game", f"*{EXE}"))
    exe = next((e for e in exe_candidates if os.path.isfile(e) and
                (e.endswith(".exe") if IS_WIN else os.access(e, os.X_OK))), None)
    return {"dir": port, "id": gid, "toml": tomls[0], "local": local, "local_path": local_path, "exe": exe}


def save_local(info: dict) -> None:
    os.makedirs(os.path.dirname(info["local_path"]), exist_ok=True)
    with open(info["local_path"], "w") as f:
        json.dump(info["local"], f, indent=2)


def dcdisc_env() -> dict:
    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.join(ROOT, "engine", "tools", "dcdisc") + os.pathsep + env.get("PYTHONPATH", "")
    return env


# ---------------------------------------------------------------------------------------------
# commands

def cmd_doctor(_a) -> int:
    env = build_env()
    rows = []

    def have(name, ok, note=""):
        rows.append((name, "ok" if ok else "MISSING", note))

    for tool in ("cmake", "ninja"):
        have(tool, shutil.which(tool, path=env.get("PATH")) is not None)
    ccs = ("clang-cl",) if IS_WIN else ("clang++", "g++")
    have("C++20 compiler", any(shutil.which(c, path=env.get("PATH")) for c in ccs), "/".join(ccs))
    if IS_WIN:
        have("MSVC environment (vcvars64)", vcvars() is not None, "Visual Studio 2022 Build Tools: Windows SDK + linker")
    glslc = shutil.which("glslc", path=env.get("PATH")) or (
        os.path.join(env["VULKAN_SDK"], "Bin", "glslc.exe") if env.get("VULKAN_SDK") else None)
    have("Vulkan SDK / glslc", bool(glslc and os.path.exists(glslc)), "Vulkan headers, loader and glslc")
    have("SDL3", bool(glob.glob(os.path.join(DEPS, "SDL3-*"))) or not IS_WIN, "python tools/dc.py deps")
    w = max(len(r[0]) for r in rows)
    for name, st, note in rows:
        print(f"{name:<{w}}  {st:<7}  {note}")
    return 0 if all(r[1] == "ok" for r in rows) else 1


def cmd_deps(_a) -> int:
    os.makedirs(DEPS, exist_ok=True)
    if IS_WIN:
        dst = os.path.join(DEPS, f"SDL3-{SDL3_VERSION}")
        if os.path.isdir(dst):
            print(f"SDL3 {SDL3_VERSION} already in {dst}")
            return 0
        url = (f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL3_VERSION}/"
               f"SDL3-devel-{SDL3_VERSION}-VC.zip")
        zpath = os.path.join(DEPS, "sdl3.zip")
        print(f"downloading {url}")
        urllib.request.urlretrieve(url, zpath)
        with zipfile.ZipFile(zpath) as z:
            z.extractall(DEPS)
        os.remove(zpath)
        print(f"SDL3 -> {dst}")
        return 0
    print("Linux/macOS: install SDL3 from your package manager (e.g. `brew install sdl3`, "
          "`apt install libsdl3-dev`) or build it from source; CMake finds it on the default prefix.")
    return 0


def cmd_setup(a) -> int:
    info = port_info()
    disc = os.path.abspath(a.disc)
    from_toml = open(info["toml"], encoding="utf-8").read()
    want = None
    for line in from_toml.splitlines():
        if line.strip().startswith("sha1_1st_read"):
            want = line.split("=", 1)[1].split("#")[0].strip().strip('"')
    # Only the boot executable is extracted: the translator needs it at build time, and at run
    # time everything else is read off the disc image itself.
    sys.path.insert(0, os.path.join(ROOT, "engine", "tools", "dcdisc"))
    from dcdisc import open_image
    from dcdisc.ipbin import parse_ipbin, read_ipbin
    from dcdisc.iso9660 import Iso9660
    from dcdisc.scramble import descramble
    # The images the config accepts ([[disc.chd]]): when listed, only those, checked by the SHA-1
    # the CHD's header records, then the whole image's data against the header.
    import tomllib
    disc_cfg = tomllib.loads(from_toml).get("disc", {})
    accepted = [(e["sha1"].lower(), e.get("name", "")) for e in disc_cfg.get("chd", [])]
    if disc_cfg.get("chd_sha1"):
        accepted.append((disc_cfg["chd_sha1"].lower(), ""))
    if accepted:
        from dcdisc.chd import ChdError, data_sha1, read_header
        names = "\n".join(f"  {n or '?'}.chd (SHA-1 {s})" for s, n in accepted)
        try:
            header = read_header(disc)
        except (ChdError, OSError) as e:
            sys.exit(f"{disc} is not a supported image ({e}). Rez runs only from:\n{names}")
        got_chd = header.sha1.hex()
        match = next((n or s for s, n in accepted if s == got_chd), None)
        if match is None:
            sys.exit(f"{disc} is not a supported image (CHD SHA-1 {got_chd}). Rez runs only from:\n{names}")
        print(f"disc: {match}; checking the whole image against its header ...")
        if data_sha1(disc) != header.raw_sha1:
            sys.exit(f"{disc}: the data does not match its header's checksum; the file is damaged")
        print("disc: image intact")
    # Overlays the config names (`[[relocations]] file = "extracted/fs/<disc path>"`): code the
    # program reads from disc at run time, which the translator needs at build time as well.
    import re
    overlay_paths = sorted(set(re.findall(r'^\s*file\s*=\s*"extracted/fs/([^"]+)"', from_toml, re.M)))
    overlays = {}
    with open_image(disc) as img:
        ip = parse_ipbin(read_ipbin(img))
        iso = Iso9660(img)
        data = iso.read_path(ip.boot_filename)
        for p in overlay_paths:
            overlays[p] = iso.read_path(p)
    if ip.hardware_id.startswith("SEGA SEGAKATANA") is False:
        print(f"warning: IP.BIN hardware id is {ip.hardware_id!r}")
    got = hashlib.sha1(data).hexdigest()
    if want and got != want and hashlib.sha1(descramble(data)).hexdigest() == want:
        data, got = descramble(data), want  # MIL-CD style scrambled boot file
    print(f"{ip.title} {ip.product_number} {ip.product_version}: boot file {ip.boot_filename}, "
          f"{len(data)} bytes")
    # Checked before anything is written, so a disc of another release leaves the existing
    # extraction and remembered disc as they were.
    if want and got != want:
        sys.exit(f"boot binary SHA-1 {got} does not match the config ({want}): wrong release or revision; "
                 "nothing was written")
    print(f"boot binary verified: sha1 {got}")
    out = os.path.join(info["dir"], "game", "extracted", "fs")
    os.makedirs(out, exist_ok=True)
    boot = os.path.join(out, "1ST_READ.BIN")

    def write_if_changed(dst: str, blob: bytes) -> None:
        # An unchanged file keeps its time stamp, so the build does not translate again.
        if os.path.exists(dst) and open(dst, "rb").read() == blob:
            return
        with open(dst, "wb") as f:
            f.write(blob)

    write_if_changed(boot, data)
    print(f"boot file -> {boot}")
    for p, blob in overlays.items():
        dst = os.path.join(out, *p.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        write_if_changed(dst, blob)
    if overlays:
        print(f"{len(overlays)} overlay file(s) named in the config extracted to {out}")
    info["local"]["disc"] = disc
    save_local(info)
    print(f"disc path remembered in {info['local_path']}")
    return 0


def cmd_build(a) -> int:
    info = port_info()
    bdir = os.path.join(info["dir"], "build")
    if a.fresh and os.path.isdir(bdir):
        shutil.rmtree(bdir)
    if not os.path.exists(os.path.join(bdir, "CMakeCache.txt")):
        cfg = ["cmake", "-S", info["dir"], "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
        if IS_WIN:
            cfg += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
        if a.headless:
            cfg += ["-DDREAM_RENDERER=OFF"]
        run(cfg)
    target = a.target or f"{info['id']}_boot"
    run(["cmake", "--build", bdir, "--target", target, "--parallel"])
    return 0


def launcher_args(info: dict, extra: list[str]) -> list[str]:
    exe = info["exe"]
    if not exe:
        sys.exit("no executable yet: python tools/dc.py build")
    args = [exe, "--config", info["toml"]]
    if info["local"].get("disc") and "--disc" not in extra:
        args += ["--disc", info["local"]["disc"]]
    return args + extra


def cmd_run(a) -> int:
    info = port_info()
    extra = list(a.args)
    if extra and extra[0] == "--":
        extra = extra[1:]
    if a.window:
        extra = ["--window"] + extra
    return run(launcher_args(info, extra), cwd=info["dir"], check=False, env=dict(os.environ))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("doctor").set_defaults(fn=cmd_doctor)
    sub.add_parser("deps").set_defaults(fn=cmd_deps)
    s = sub.add_parser("setup")
    s.add_argument("--disc", required=True)
    s.set_defaults(fn=cmd_setup)
    s = sub.add_parser("build")
    s.add_argument("--target")
    s.add_argument("--fresh", action="store_true")
    s.add_argument("--headless", action="store_true")
    s.set_defaults(fn=cmd_build)
    s = sub.add_parser("run")
    s.add_argument("--window", action="store_true")
    s.add_argument("args", nargs=argparse.REMAINDER)
    s.set_defaults(fn=cmd_run)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
