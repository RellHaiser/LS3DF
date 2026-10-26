"""Builds the LS3DF.dll port (x86, MSVC).

Usage: python build.py [dll] [check] [test] [--debug] [--only SUBSTR]
  dll    (default) compile everything and link build/msvc/LS3DF.dll with ls3df.def
  check  compile every translation unit, no link (fast syntax/type check while porting)
  test   build tests/*.cpp into build/<name>.exe
Intermediate files go to %TEMP%\\ls3df_build: a synced source folder (Google Drive, OneDrive) locks
freshly written files while it uploads them, which breaks cl and link.
Needs Visual Studio with the C++ x86 tools (found with vswhere) and MinGW's dlltool (%DLLTOOL%, PATH
or a common install) for the import libraries of lib/*.def.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
WORK = os.path.join(tempfile.gettempdir(), "ls3df_build")
LIBS = ["kernel32.lib", "user32.lib", "gdi32.lib", "ole32.lib", "winmm.lib", "version.lib", "advapi32.lib",
        "dinput8.lib", "dxguid.lib", "dsound.lib", "strmiids.lib"]
# Import libraries made from lib/*.def (binkw32, vorbisfile, rw_data): MSVC's lib.exe cannot keep
# the leading underscore of decorated __stdcall export names ("_BinkOpen@8"), MinGW dlltool can.
IMPORT_DEFS = ["binkw32", "rw_data", "vorbisfile"]
# Where to look for it: %DLLTOOL%, PATH, then common 32-bit MinGW installs (MSYS2, Nim's own).
DLLTOOL_PATHS = [r"C:\msys64\mingw32\bin\dlltool.exe", r"C:\mingw32\bin\dlltool.exe",
                 r"C:\nim-2.2.6\dist\mingw32\bin\dlltool.exe"]


def sources(only=None):
    out = []
    for d, _, files in os.walk(SRC):
        for f in files:
            if f.endswith(".cpp"):
                p = os.path.join(d, f)
                if not only or only in p.replace("\\", "/"):
                    out.append(p)
    return sorted(out)


def find_vcvars():
    if os.path.exists(VSWHERE):
        r = subprocess.run([VSWHERE, "-latest", "-products", "*", "-requires",
                            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                            "installationPath"], capture_output=True, text=True)
        for path in r.stdout.splitlines():
            bat = os.path.join(path.strip(), r"VC\Auxiliary\Build\vcvars32.bat")
            if path.strip() and os.path.exists(bat):
                return bat
    return None


_env = None


def msvc_env():
    """The environment vcvars32.bat sets up (computed once)."""
    global _env
    if _env is None:
        bat = find_vcvars()
        if not bat:
            sys.exit("msvc: Visual Studio with the C++ x86 tools not found")
        r = subprocess.run(f'"{bat}" >nul 2>nul && set', shell=True, capture_output=True, text=True)
        env = dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)
        path = env.get("PATH") or env.get("Path", "")
        for tool in ("cl", "link"):
            env["_" + tool.upper()] = shutil.which(tool, path=path) or tool
        _env = env                      # complete before other threads can see it
    return _env


def tool(name):
    return msvc_env()["_" + name.upper()]


def cflags(debug):
    return ["/nologo", "/c", "/EHsc", "/std:c++17", "/W4", "/permissive-", "/MT" + ("d" if debug else ""),
            "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/DLS3D_API=", "/Zi",
            "/Od" if debug else "/O2", "/DNDEBUG" if not debug else "/D_DEBUG",
            "/wd4100",                      # unreferenced parameters (interface methods)
            "/I" + SRC, "/I" + os.path.join(ROOT, "include")]


# The render device seam (docs/render_device.md): only the Direct3D 8 backend and the Direct3D
# declarations may name Direct3D's interfaces or include them; the rest of the engine draws
# through gfx::Device.
SEAM_PATTERN = re.compile(r'#include "dx/d3d8\.h"|#include "gfx/d3d8/|\bIDirect3D\w*8\b|\bg_pD3D\b|\bg_pDevice\b')
SEAM_ALLOWED = ("dx/", "gfx/d3d8/")


def check_seams():
    """Lines outside the backends that use what only a backend may (printed); True when none."""
    bad = []
    for d, _, files in os.walk(SRC):
        for f in files:
            path = os.path.join(d, f)
            rel = os.path.relpath(path, SRC).replace("\\", "/")
            if not f.endswith((".cpp", ".h")) or rel.startswith(SEAM_ALLOWED):
                continue
            for n, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                if SEAM_PATTERN.search(line):
                    bad.append("%s(%d): uses the Direct3D 8 backend: %s" % (rel, n, line.strip()))
    for line in bad:
        print(line)
    return not bad


def compile_one(src, debug):
    rel = os.path.relpath(src, SRC)
    obj = os.path.join(WORK, "obj", re.sub(r"[\\/]", "_", rel)[:-4] + ".obj")
    pdb = os.path.join(WORK, "obj", "ls3df.pdb")
    cmd = [tool("cl"), *cflags(debug), "/FS", "/Fd" + pdb, "/Fo" + obj, src]
    r = subprocess.run(cmd, capture_output=True, text=True, env=msvc_env())
    text = "\n".join(l for l in (r.stdout + r.stderr).splitlines()
                     if l.strip() and l.strip() != os.path.basename(src))
    return src, obj, r.returncode == 0, text


def compile_all(debug, only=None):
    os.makedirs(os.path.join(WORK, "obj"), exist_ok=True)
    srcs = sources(only)
    ok = True
    objs = []
    with ThreadPoolExecutor(max(2, os.cpu_count() or 4)) as ex:
        for src, obj, good, text in ex.map(lambda s: compile_one(s, debug), srcs):
            if text:
                print(text)
            ok &= good
            objs.append(obj)
    print("compiled %d files: %s" % (len(srcs), "ok" if ok else "ERRORS"))
    return ok, objs


def import_libs():
    """WORK/<name>.lib for each lib/<name>.def (remade when the .def is newer)."""
    dlltool = (os.environ.get("DLLTOOL") or shutil.which("dlltool")
               or next((p for p in DLLTOOL_PATHS if os.path.exists(p)), None))
    out = []
    for name in IMPORT_DEFS:
        src = os.path.join(ROOT, "lib", name + ".def")
        lib = os.path.join(WORK, name + ".lib")
        if not os.path.exists(lib) or os.path.getmtime(lib) < os.path.getmtime(src):
            if not dlltool:
                sys.exit("import libraries: MinGW dlltool not found (set DLLTOOL to its path)")
            os.makedirs(WORK, exist_ok=True)
            subprocess.run([dlltool, "-m", "i386", "-d", src, "-l", lib], check=True)
        out.append(lib)
    return out


def link(objs, debug):
    dll = os.path.join(WORK, "LS3DF.dll")
    cmd = [tool("link"), "/nologo", "/DLL", "/DEBUG", "/INCREMENTAL:NO", "/MACHINE:X86",
           "/DEF:" + os.path.join(ROOT, "ls3df.def"), "/OUT:" + dll, *objs, *LIBS, *import_libs()]
    r = subprocess.run(cmd, capture_output=True, text=True, env=msvc_env())
    text = "\n".join(l for l in (r.stdout + r.stderr).splitlines() if l.strip() and "Creating library" not in l)
    if text:
        print(text)
    if r.returncode != 0:
        print("link: FAILED")
        return False
    out = os.path.join(ROOT, "build", "msvc")
    os.makedirs(out, exist_ok=True)
    for name in ("LS3DF.dll", "LS3DF.lib", "LS3DF.pdb"):
        if os.path.exists(os.path.join(WORK, name)):
            shutil.copyfile(os.path.join(WORK, name), os.path.join(out, name))
    print("link: ok ->", os.path.join(out, "LS3DF.dll"))
    return True


def test_directives(path):
    """`// test-sources: src/igraph/*.cpp ...` lines of a test: port sources it links."""
    import glob
    srcs, libs = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*//\s*test-(sources|libs):\s*(.*)", line)
        if m:
            for pat in m.group(2).split():
                found = sorted(glob.glob(os.path.join(ROOT, pat)))
                (srcs if m.group(1) == "sources" else libs).extend(found or [pat])
    return srcs, libs


def build_tests(debug, only=None):
    ok = True
    tdir = os.path.join(ROOT, "tests")
    out = os.path.join(ROOT, "build")
    os.makedirs(out, exist_ok=True)
    for f in sorted(os.listdir(tdir)) if os.path.isdir(tdir) else []:
        if not f.endswith(".cpp") or (only and only not in f):
            continue
        exe = os.path.join(WORK, f[:-4] + ".exe")
        flags = [x for x in cflags(debug) if x != "/c"]
        extra, extra_libs = test_directives(os.path.join(tdir, f))
        objs = []
        if extra:
            # Port sources compiled for the test (same flags as the DLL).
            os.makedirs(os.path.join(WORK, "obj"), exist_ok=True)
            with ThreadPoolExecutor(max(2, os.cpu_count() or 4)) as ex:
                for src, obj, good, text in ex.map(lambda s: compile_one(s, debug), extra):
                    if text:
                        print(text)
                    ok &= good
                    objs.append(obj)
        cmd = [tool("cl"), *flags, "/Fo" + WORK + "\\", "/Fd" + os.path.join(WORK, f[:-4] + ".pdb"), "/Fe" + exe,
               os.path.join(tdir, f), *objs, "/link", "/INCREMENTAL:NO", *LIBS, *import_libs(), *extra_libs]
        r = subprocess.run(cmd, capture_output=True, text=True, env=msvc_env())
        text = "\n".join(l for l in (r.stdout + r.stderr).splitlines() if l.strip() and l.strip() != f)
        if text:
            print(text)
        if r.returncode == 0:
            shutil.copyfile(exe, os.path.join(out, f[:-4] + ".exe"))
        ok &= r.returncode == 0
        print("test %s: %s" % (f, "ok" if r.returncode == 0 else "FAILED"))
    return ok


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    debug = "--debug" in sys.argv
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    if only in args:
        args.remove(only)
    targets = args or ["dll"]
    ok = True
    if "check" in targets or "dll" in targets:
        ok &= check_seams()
    if "check" in targets:
        ok &= compile_all(debug, only)[0]
    if "dll" in targets:
        good, objs = compile_all(debug)
        ok &= good and link(objs, debug)
    if "test" in targets:
        ok &= build_tests(debug, only)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
