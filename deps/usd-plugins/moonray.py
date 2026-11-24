"""The build of the moonray pack, which isn't one CMake project (docs/MOONRAY.md).

tools/build_usd_plugin.py calls build() instead of its own configure, build and
install when deps/usd-plugins/moonray.json says "hook": "moonray.py". By then the
openmoonray checkout has the submodules the pin lists, with their patches, and
the pin's downloads are unpacked.

1. The dependencies the OpenUSD asset doesn't have, from the downloads, into
   <work>/deps: JsonCpp, log4cplus, Lua, Random123, ISPC, OpenSSL (static),
   CppUnit (only because scene_rdl2's CMake asks for it), Open Image Denoise,
   libcurl, FreeType (static, on macOS alone), libmicrohttpd (only to build arras4_core; the one library that links
   it, its HTTP server, is left out of the pack) and Boost.ProgramOptions, which the asset's Boost lacks. Everything the asset
   has (TBB, Embree, OpenImageIO, OpenEXR, OpenVDB, OpenSubdiv, Boost) is the
   asset's.
2. MoonRay's own projects, one after the other, into <work>/install.
3. What runs, into the pack: plugin/usd/moonray/{bin,lib,rdl2dso,sessions,plugin},
   and a plugInfo.json per plugin in plugin/usd/<plugin>/resources, where OpenUSD's
   plugin/usd/plugInfo.json finds it.
   The plugins set the variables MoonRay reads from where they are
   (moonray-hdmoonray-26.08.patch, plugin/common/Environment.cc).

Linux x64 and macOS on Apple silicon (docs/MOONRAY.md section 7.6). Returns the licences of the dependencies that ship, as {name: folder}.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

MACOS = sys.platform == "darwin"

# MoonRay's projects, in build order: (folder in the checkout, extra CMake arguments).
PROJECTS = [
    ("moonray/scene_rdl2", []),
    ("moonray/mcrt_denoise", []),
    ("moonray/moonray", []),
    ("moonray/moonshine", []),
    ("arras/arras4_core", []),
    ("moonray/moonray_arras/mcrt_messages", []),
    ("moonray/moonray_arras/mcrt_dataio", []),
    ("moonray/moonray_arras/mcrt_computation", []),
    ("moonray/hydra/hdMoonray", ["-DHDMOONRAY_ARRAS=ON", "-DHDMOONRAY_TOOLS=OFF"]),
]
# The dependencies whose code is in the pack, as a library or linked in.
SHIPPED = ["jsoncpp", "log4cplus", "lua", "random123", "openssl", "oidn", "curl", "boost"]
# Where the pack's MoonRay goes, and the relative RUNPATHs (rpaths) its binaries get.
ROOT = "plugin/usd/moonray"
TO_PACK_LIB = "$ORIGIN/../../../../lib"  # from <root>/<folder>/ to the install's lib

# https://github.com/xoolive/geodesy/blob/master/license.txt
GEODESY = """The MIT License (MIT)

Copyright (c) 2015 Xavier Olive

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""


def build(ctx):
    run, places, work, usd = ctx.run, ctx.places, ctx.work, ctx.usd
    deps = work / "deps"
    install = work / "install"
    jobs = str(ctx.jobs or os.cpu_count() or 4)
    for folder in (deps / "bin", deps / "lib", deps / "include"):
        folder.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PATH=f"{deps / 'bin'}:{install / 'bin'}:{os.environ['PATH']}",
               **({} if MACOS else {"LD_LIBRARY_PATH": f"{usd / 'lib'}:{deps / 'lib'}:{install / 'lib'}"}))

    def cmake(name, source, *arguments, generator="Ninja", build_dir=None):
        build_dir = build_dir or work / "build" / name
        run(["cmake", "-S", source, "-B", build_dir, "-G", generator, "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DCMAKE_INSTALL_LIBDIR=lib", *arguments], env=env)
        run(["cmake", "--build", build_dir, "--parallel", jobs], env=env)
        run(["cmake", "--install", build_dir], env=env)

    def done(name):
        return (work / "build" / f".{name}-done").is_file()

    def mark(name):
        (work / "build").mkdir(parents=True, exist_ok=True)
        (work / "build" / f".{name}-done").touch()

    prefix = f"-DCMAKE_INSTALL_PREFIX={deps}"
    if not done("jsoncpp"):
        cmake("jsoncpp", places["jsoncpp"], prefix, "-DJSONCPP_WITH_TESTS=OFF", "-DBUILD_SHARED_LIBS=ON",
              "-DBUILD_STATIC_LIBS=OFF", "-DBUILD_OBJECT_LIBS=OFF", "-DJSONCPP_WITH_PKGCONFIG_SUPPORT=OFF")
        mark("jsoncpp")
    if not done("log4cplus"):
        cmake("log4cplus", places["log4cplus"], prefix, "-DLOG4CPLUS_BUILD_TESTING=OFF",
              "-DLOG4CPLUS_BUILD_LOGGINGSERVER=OFF", "-DWITH_UNIT_TESTS=OFF", "-DUNICODE=OFF")
        mark("log4cplus")
    if not done("lua"):
        # luac too: scene_rdl2 compiles its Lua library with it, and without it the
        # build goes on with an empty one (docs/MOONRAY.md section 7.1).
        lua = Path(places["lua"])
        run(["make", "-C", lua, "macosx" if MACOS else "linux", "MYCFLAGS=-fPIC", f"-j{jobs}"], env=env)
        for name in ("lua", "luac"):
            shutil.copy2(lua / "src" / name, deps / "bin")
        shutil.copy2(lua / "src" / "liblua.a", deps / "lib")
        for name in ("lua.h", "luaconf.h", "lualib.h", "lauxlib.h", "lua.hpp"):
            shutil.copy2(lua / "src" / name, deps / "include")
        mark("lua")
    if not done("random123"):
        # Headers only; its Makefile's install needs GNU cp.
        shutil.copytree(Path(places["random123"]) / "include" / "Random123", deps / "include" / "Random123", dirs_exist_ok=True)
        mark("random123")
    if not done("ispc"):
        shutil.copy2(Path(places["ispc"]) / "bin" / "ispc", deps / "bin")
        mark("ispc")
    if not done("openssl"):
        openssl = Path(places["openssl"])
        run(["./Configure", "no-shared", "no-tests", "-fPIC", f"--prefix={deps}", "--libdir=lib"], cwd=openssl, env=env)
        run(["make", f"-j{jobs}"], cwd=openssl, env=env)
        run(["make", "install_dev"], cwd=openssl, env=env)
        mark("openssl")
    if not done("cppunit"):
        cppunit = Path(places["cppunit"])
        run(["./configure", f"--prefix={deps}", "--disable-static", "--disable-doxygen"], cwd=cppunit, env=env)
        run(["make", f"-j{jobs}", "install"], cwd=cppunit, env=env)
        mark("cppunit")
    if not done("oidn"):
        cmake("oidn", places["oidn"], prefix, f"-DISPC_EXECUTABLE={deps / 'bin' / 'ispc'}", f"-DTBB_ROOT={usd}",
              f"-DTBB_DIR={usd / 'lib' / 'cmake' / 'TBB'}", "-DOIDN_APPS=OFF")
        mark("oidn")
    if not done("curl"):
        cmake("curl", places["curl"], prefix, f"-DOPENSSL_ROOT_DIR={deps}", "-DOPENSSL_USE_STATIC_LIBS=ON",
              "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DHTTP_ONLY=ON", "-DCURL_USE_LIBPSL=OFF",
              "-DCURL_USE_LIBSSH2=OFF", "-DUSE_LIBIDN2=OFF", "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF",
              "-DUSE_NGHTTP2=OFF", "-DBUILD_LIBCURL_DOCS=OFF", "-DBUILD_MISC_DOCS=OFF", "-DENABLE_CURL_MANUAL=OFF")
        mark("curl")
    if MACOS and not done("freetype"):
        # Not the system's, as on Linux. Static: usdcc's Qt has a libfreetype of its own.
        cmake("freetype", places["freetype"], prefix, "-DBUILD_SHARED_LIBS=OFF", "-DFT_DISABLE_ZLIB=ON",
              "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_PNG=ON", "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_BROTLI=ON")
        mark("freetype")
    if not done("microhttpd"):
        mhd = Path(places["microhttpd"])
        run(["./configure", f"--prefix={deps}", "--disable-https", "--disable-doc", "--disable-examples"], cwd=mhd, env=env)
        run(["make", f"-j{jobs}", "install"], cwd=mhd, env=env)
        mark("microhttpd")
    boost = Path(places["boost"])
    if not done("boost"):
        # The asset's Boost (1.80, on macOS 1.82) without ProgramOptions, which scene_rdl2 links.
        run(["./bootstrap.sh", "--with-libraries=program_options"], cwd=boost, env=env)
        run(["./b2", f"-j{jobs}", "link=shared", "variant=release", "threading=multi",
             # Room for the rpath it gets below.
             *(["linkflags=-Wl,-headerpad_max_install_names"] if MACOS else []),
             f"--prefix={deps}", "--libdir=" + str(deps / "lib"), "install"], cwd=boost, env=env)
        mark("boost")

    # MoonRay.
    flags = " ".join([
        # Headers GCC 13 no longer includes on the way (docs/MOONRAY.md section 7.1).
        "-include functional", "-include cstdint", "-include memory", "-include string",
        # Boost headers the asset leaves out (boost/algorithm), after its own.
        f"-idirafter {boost}", f"-idirafter {deps / 'include'}"])
    if MACOS:
        sdk = Path(subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip())
        platform = [
            "-DMOONRAY_USE_METAL=NO",  # CPU only, as on Linux
            f"-DLibcurl_LIBRARY={deps / 'lib' / 'libcurl.dylib'}",
            # Not CMake's own ISPC support, which hands it Clang's flags: MoonRay's custom
            # commands, which upstream's build for macOS (Xcode) uses too.
            "-DCMAKE_XCODE_BUILD_SYSTEM=12", "-DCMAKE_ISPC_COMPILER=",
            # libuuid and the atomics are in libSystem; ArrasCore's package asks for both.
            f"-DLibatomic_LIBRARY={sdk / 'usr/lib/libSystem.tbd'}",
            f"-DLibuuid_INCLUDE_DIRS={sdk / 'usr/include/uuid'}", f"-DLibuuid_LIBRARY={sdk / 'usr/lib/libSystem.tbd'}",
        ]
    else:
        no_as_needed = "-Wl,--no-as-needed"  # MoonRay's libraries need each other in circles
        platform = [
            "-DMOONRAY_USE_OPTIX=NO", f"-DLibcurl_LIBRARY={deps / 'lib' / 'libcurl.so.4'}",
            f"-DCMAKE_SHARED_LINKER_FLAGS={no_as_needed}", f"-DCMAKE_EXE_LINKER_FLAGS={no_as_needed}",
            f"-DCMAKE_MODULE_LINKER_FLAGS={no_as_needed}",
        ]
    program_options = next((deps / "lib" / "cmake").glob("boost_program_options-*"))
    common = [
        f"-DCMAKE_INSTALL_PREFIX={install}", f"-DCMAKE_PREFIX_PATH={install};{deps};{usd}",
        f"-DCMAKE_MODULE_PATH={ctx.source / 'cmake_modules' / 'cmake'}",
        f"-Dboost_program_options_DIR={program_options}",
        f"-DPython_ROOT_DIR={ctx.python_root}", f"-DPython3_ROOT_DIR={ctx.python_root}",
        "-DBUILD_TESTING=OFF", "-DABI_VERSION=0", "-DBUILD_QT_APPS=NO",
        f"-DCMAKE_CXX_FLAGS={flags}", f"-DLibcurl_INCLUDE_DIR={deps / 'include'}", f"-DLUA_DIR={deps}", *platform,
    ]
    env.update({name: str(deps) for name in (
        "JsonCpp_ROOT", "JSONCPP_ROOT", "Log4cplus_ROOT", "LOG4CPLUS_ROOT", "CppUnit_ROOT", "CPPUNIT_ROOT",
        "Random123_ROOT", "RANDOM123_ROOT", "LIBMICROHTTPD_ROOT", "LIBCURL_ROOT", "Libcurl_ROOT",
        "OPENSSL_ROOT_DIR", "LUA_DIR")})
    env.update({"OpenVDB_ROOT": str(usd), "OPENVDB_ROOT": str(usd), "OpenSubDiv_ROOT": str(usd),
                "OPENSUBDIV_ROOT": str(usd), "TBB_ROOT": str(usd), "ISPC": str(deps / "bin" / "ispc"),
                "CMAKE_MODULES_ROOT": str(ctx.source / "cmake_modules")})
    for folder, extra in PROJECTS:
        name = folder.rsplit("/", 1)[-1]
        if MACOS and folder == "moonray/moonray":
            # scene_rdl2's headers include these, which its install leaves out.
            arm = Path("common/arm")
            shutil.copytree(ctx.source / "moonray/scene_rdl2/lib" / arm, install / "include/scene_rdl2" / arm, dirs_exist_ok=True)
        if done(name):
            continue
        # Unix Makefiles: with Ninja their ISPC headers aren't made before what includes them.
        cmake(name, ctx.source / folder, *common, *extra, generator="Unix Makefiles")
        mark(name)
    rdlalib = work / "build" / "scene_rdl2" / "lib" / "scene" / "rdl2" / "rdlalib.cc"
    if rdlalib.stat().st_size < 10000:
        raise SystemExit(f"{rdlalib} is nearly empty: scene_rdl2 was built without luac")

    # What runs, into the pack.
    root = ctx.prefix / ROOT
    for folder in ("bin", "lib", "rdl2dso", "sessions", "plugin"):
        (root / folder).mkdir(parents=True, exist_ok=True)
    shutil.copy2(install / "bin" / "execComp", root / "bin")
    for name in ("libjsoncpp", "liblog4cplus", "libOpenImageDenoise", "libcurl", "libboost_program_options"):
        for file in sorted((deps / "lib").glob(name + ("*.dylib" if MACOS else "*.so*"))):
            target = root / "lib" / file.name
            if file.is_symlink():
                target.symlink_to(os.readlink(file))
            else:
                shutil.copy2(file, target)
    for file in sorted((install / "rdl2dso").iterdir()):
        # Not the .so.proxy files, which only MoonRay's tools read, nor its test shaders.
        if file.name.endswith(".so") and not file.name.startswith("Test"):
            shutil.copy2(file, root / "rdl2dso")
    loaded = []  # the computations Arras opens by name, from the sessions
    for name in ("hd_single.sessiondef", "hd_multi.sessiondef"):
        shutil.copy2(install / "sessions" / name, root / "sessions")
        loaded += re.findall(r'"dso"\s*:\s*"([^"]+)"', (install / "sessions" / name).read_text())
    # A plugInfo.json each, in plugin/usd/<plugin>/resources: OpenUSD registers only the
    # first plugin of a file that lists several.
    for info in sorted((install / "plugin" / "pxr").glob("*/plugInfo.json")):
        plugins = json.loads(info.read_text())["Plugins"]
        library = (info.parent / plugins[0]["LibraryPath"]).resolve()
        shutil.copy2(library, root / "plugin")
        for plugin in plugins:
            plugin["LibraryPath"] = f"../../moonray/plugin/{library.name}"
        resources = ctx.prefix / "plugin" / "usd" / plugins[0]["Name"] / "resources"
        resources.mkdir(parents=True, exist_ok=True)
        (resources / "plugInfo.json").write_text(json.dumps({"Plugins": plugins}, indent=4) + "\n")

    if MACOS:
        from _macho import edit, install_id, is_system, macho_files as binaries, needed as links, sign

        def needed(file):
            return [Path(name).name for name in links(file) if not is_system(name)]
    else:
        from _elf import elf_files as binaries, needed, set_runpath
    # Of MoonRay's own libraries only those something above loads: what the plugins,
    # execComp, the shaders and the sessions' computations link, and so on. That leaves
    # out its tests' libraries and Arras's HTTP server, libhttpserver, which links
    # libmicrohttpd (LGPL-2.1), not in the pack.
    available = {file.name: file for file in (install / "lib").iterdir()
                 if file.suffix == (".dylib" if MACOS else ".so") and not file.is_symlink()}
    missing = [name for name in loaded if name not in available]
    if missing:
        raise SystemExit(f"the sessions name libraries the build didn't make: {', '.join(missing)}")
    kept = set(loaded)
    todo = [*binaries(root), *(available[name] for name in loaded)]
    while todo:
        for name in needed(todo.pop()):
            if name in available and name not in kept:
                kept.add(name)
                todo.append(available[name])
    for name in sorted(kept):
        shutil.copy2(available[name], root / "lib")
    print(f"moonray: {len(kept)} of MoonRay's {len(available)} libraries ship; left out: "
          + ", ".join(sorted(set(available) - kept)))

    # macOS: what links a library by a link's name (libjsoncpp.25.dylib) links it by its own
    # instead, as the hardened runtime wants, and the links aren't shipped.
    own_names = {}
    for file in (root / "lib").iterdir() if MACOS else ():
        if file.is_symlink():
            own_names[file.name] = file.resolve().name
            file.unlink()
    for file in binaries(root):
        own_lib = "" if file.parent.name == "lib" else "/../lib"
        if MACOS:
            run(["strip", "-x", file])
            edit(file, new_rpaths=["@loader_path" + own_lib, TO_PACK_LIB.replace("$ORIGIN", "@loader_path")],
                 new_id="@rpath/" + file.name if install_id(file) else None,
                 changes={name: "@rpath/" + own_names.get(Path(name).name, Path(name).name) for name in links(file)
                          if not is_system(name)})
            sign(file)
        else:
            set_runpath(file, ["$ORIGIN" + own_lib, TO_PACK_LIB])
            run(["strip", "--strip-unneeded", file])
    licences = {name: Path(places[name]) for name in SHIPPED}
    # Two that have no file of their own the build would find by its name: Lua's
    # notice is at the end of lua.h, and MoonRay's THIRD-PARTY.md reproduces the
    # licences of the code it took from Embree, OpenImageIO, OSL, pbrt and others.
    notices = work / "notices"
    lua_h = next(Path(places["lua"]).glob("**/src/lua.h"))
    notice = re.search(r"\* (Copyright \(C\) 1994-.*?SOFTWARE\.)\n\*{10}", lua_h.read_text(), re.S)
    if not notice:
        raise SystemExit(f"no copyright notice at the end of {lua_h}")
    (notices / "lua").mkdir(parents=True, exist_ok=True)
    (notices / "lua" / "LICENSE").write_text(re.sub(r"(?m)^\*( |$)", "", notice.group(1)) + "\n")
    licences["lua"] = notices / "lua"
    (notices / "moonray").mkdir(parents=True, exist_ok=True)
    shutil.copy2(ctx.source / "THIRD-PARTY.md", notices / "moonray" / "LICENSE-THIRD-PARTY.md")
    # Two that THIRD-PARTY.md leaves out (docs/MOONRAY.md section 3.1): the notice of the
    # OSL release its light path expressions are from, at the top of those sources, and
    # xoolive/geodesy's, whose atan ssef.h and avxf.h have.
    osl = ctx.source / "moonray/moonray/lib/rendering/lpe/osl"
    found = {notice.group(1) for file in sorted(osl.iterdir()) if file.suffix in (".cc", ".h")
             if (notice := re.search(r"/\*\n(Copyright [^\n]*Sony Pictures Imageworks.*?)\n\*/", file.read_text(), re.S))}
    if len(found) != 1:
        raise SystemExit(f"{len(found)} different Sony Pictures Imageworks notices in {osl}, not one")
    (notices / "moonray" / "LICENSE-OSL-LPE").write_text(found.pop() + "\n")
    (notices / "moonray" / "LICENSE-xoolive-geodesy").write_text(GEODESY)
    if MACOS:
        # scene_rdl2's intrinsics for Arm, whose notice is at the top of the file.
        sse2neon = ctx.source / "moonray/scene_rdl2/lib/common/arm/sse2neon.h"
        found = re.search(r"// Contributors to this work are:\n.*? \* SOFTWARE\.\n", sse2neon.read_text(), re.S)
        if not found:
            raise SystemExit(f"no notice at the top of {sse2neon}")
        (notices / "moonray" / "LICENSE-sse2neon").write_text(re.sub(r"(?m)^(//| ?/\*| \*/?) ?", "", found.group(0)))
        (notices / "freetype").mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(places["freetype"]) / "docs" / "FTL.TXT", notices / "freetype" / "LICENSE-FTL.TXT")
        licences["freetype"] = notices / "freetype"
    licences["moonray"] = notices / "moonray"
    return licences
