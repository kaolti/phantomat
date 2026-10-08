# ARM64 function hooks

Hyprland 0.56.2's `CFunctionHook::hook()` returns false outside x86_64. That allows Phantomat to load on ARM64, but opening an overview reports the misleading error about another overview plugin. The ARM64 backend uses Dobby for ARM64 instruction relocation, original-function trampolines, and unhook restoration. The existing Hyprland hook backend remains in use on other architectures.

## Build

Until the Linux fixes in [Dobby PR 305](https://github.com/jmpews/Dobby/pull/305) merge, use that PR revision to build the static archive. Dobby is an Apache-2.0 dependency. Include its license and applicable notices when distributing a plugin binary containing the archive.

The changes were tested with Hyprland 0.56.2 (`efb50993780079460b0cbed1363e2166a2de1d9f`), GCC 16.1.1, Linux 7.1.13, and 16 KiB pages on a MacBook Pro 13-inch M1/J293. Use headers matching the running compositor.

```sh
git clone https://github.com/jmpews/Dobby.git
git -C Dobby fetch origin pull/305/head
git -C Dobby checkout --detach FETCH_HEAD
cmake -S Dobby -B Dobby/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DPlugin.SymbolResolver=OFF -DDOBBY_DEBUG=OFF
cmake --build Dobby/build --target dobby_static -j2

export DOBBY_INCLUDE="$(realpath Dobby/include)"
export DOBBY_LIBRARY="$(realpath Dobby/build/libdobby.a)"
make -j2 OUT=.build/dev/spatialoverview.so
make test-tools
```

Exporting the two variables also makes them available to the installer. Alternatively, place their assignments in the ignored `.build/dobby-config.mk` file.

CMake uses the same paths as cache options:

```sh
cmake -S . -B .build/cmake -DCMAKE_BUILD_TYPE=Release \
  -DDOBBY_INCLUDE="$DOBBY_INCLUDE" -DDOBBY_LIBRARY="$DOBBY_LIBRARY"
cmake --build .build/cmake -j2
```

Meson accepts equivalent project options:

```sh
meson setup .build/meson \
  -Ddobby_include="$DOBBY_INCLUDE" -Ddobby_library="$DOBBY_LIBRARY"
meson compile -C .build/meson -j2
```

CMake and Meson produce `libspatialoverview.so` in their build directories. All three frontends detect the compiler's target architecture, require a header and static archive for ARM64 plugin builds, and hide the embedded Dobby symbols. Test tools and the Make safe-unload helper can be built without Dobby. Other architectures use the existing Hyprland backend without a Dobby dependency.

The static archive is embedded in the plugin and its symbols are hidden from the compositor's global dynamic-symbol namespace. No replacement Hyprland build is required.

## Verification

Run the repeatable build check with CMake, Meson, Ninja, GNU binutils, and the normal plugin build dependencies installed:

```sh
python3 tests/build-frontends.py "$DOBBY_INCLUDE" "$DOBBY_LIBRARY"
```

It checks missing/invalid dependency errors, directly exercises hook installation and destruction, builds all three frontends, and checks that the plugins embed the hook functions without exporting Dobby or GNU unique symbols. GNU unique symbols can prevent normal plugin unload.

Before replacing a live binary, use the repository's disposable nested sessions:

```sh
python3 tests/navigator-nested.py --plugin .build/dev/spatialoverview.so
python3 tests/popup-nested.py .build/dev/spatialoverview.so
python3 tests/multimonitor-nested.py .build/dev/spatialoverview.so
```

Run all three against the exact plugin binary and matching compositor headers. The standalone Dobby smoke test also passed integer and floating-point argument/return handling, relocated original calls, and repeated hook installation/removal with pointer-authentication/BTI compiler flags.

Use the installer's existing atomic replacement and safe-unload workflow when deploying. Keep a copy of the previous binary. Rebuild against matching headers after a Hyprland update.

## Independent workspaces

The ARM64 backend supports both the persistent canvas and transient workspace overview. For Hyprsplit-style independent workspaces, configure `canvas.enabled`, `canvas.desktop_mode`, `canvas.persistent`, `canvas.auto_float`, and `canvas.auto_place` to false, and load the monitor-local workspace bindings after Phantomat's bindings. That preserves normal tiling while the overview is closed.
