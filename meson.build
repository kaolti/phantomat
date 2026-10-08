project('spatialoverview', 'cpp',
  version: '0.1',
  default_options: ['buildtype=release'],
)

cpp_compiler = meson.get_compiler('cpp')
if cpp_compiler.has_argument('-std=c++23')
  add_global_arguments('-std=c++23', language: 'cpp')
elif cpp_compiler.has_argument('-std=c++2b')
  add_global_arguments('-std=c++2b', language: 'cpp')
else
  error('Could not configure current C++ compiler (' + cpp_compiler.get_id() + ' ' + cpp_compiler.version() + ') with required C++ standard (C++23)')
endif

add_project_arguments(
  [
    '-Wno-narrowing',
  ],
  language: 'cpp')

# The plugin is the top-level sources only (scripts/ and tests/ hold separate tools).
globber = run_command('find', '.', '-maxdepth', '1', '-name', '*.cpp', check: true)
src = globber.stdout().strip().split('\n')

version_header = custom_target(
  'plugin-version',
  output: 'PluginVersion.hpp',
  command: [
    find_program('sh'),
    files('scripts/generate-plugin-version.sh'),
    '@OUTPUT@',
    meson.project_source_root(),
  ],
  build_always_stale: true,
)

hyprland = dependency('hyprland')

if cpp_compiler.get_id() == 'gcc'
  add_project_arguments('-fno-gnu-unique', language: 'cpp')
endif

hook_dependencies = []
hook_link_depends = []
if host_machine.cpu_family() == 'aarch64'
  fs = import('fs')
  dobby_include = get_option('dobby_include')
  dobby_library = get_option('dobby_library')
  if dobby_include == '' or dobby_library == ''
    error('ARM64 builds require dobby_include and dobby_library. See docs/arm64.md.')
  endif
  dobby_include = meson.project_source_root() / dobby_include
  dobby_library = meson.project_source_root() / dobby_library
  if not fs.is_file(dobby_include / 'dobby.h') or not fs.is_file(dobby_library) or not dobby_library.endswith('.a')
    error('ARM64 builds require dobby.h and a PIC libdobby.a archive. See docs/arm64.md.')
  endif
  hook_dependencies += declare_dependency(
    include_directories: include_directories(dobby_include),
    link_args: [dobby_library, '-Wl,--exclude-libs,ALL'],
  )
  hook_link_depends += dobby_library
endif

shared_module(meson.project_name(), src + [version_header],
  include_directories: include_directories('.'),
  link_depends: hook_link_depends,
  dependencies: [
    dependency('hyprland'),
    dependency('hyprgraphics'),
    dependency('pixman-1'),
    dependency('libdrm'),
    dependency('pangocairo'),
    dependency('libinput'),
    dependency('libudev'),
    dependency('lua5.4', 'lua', version: '>=5.4'),
    dependency('wayland-server'),
    dependency('xkbcommon'),
  ] + hook_dependencies,
  install: true,
)
