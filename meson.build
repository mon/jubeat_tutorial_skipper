project('jb_tutorial_skip', 'c', 'cpp',
    default_options: [
        'cpp_std=c++23',
        'c_std=c11',
        'buildtype=release',
        'strip=true',
        'optimization=3',
    ],
    meson_version: '>=1.2.0',
)

cpp = meson.get_compiler('cpp')

# #embed under -std=c++23 is a clang extension.
add_project_arguments(
    '-Wno-c23-extensions',
    '-Wno-c2x-extensions',
    '-Wno-gnu-statement-expression',
    language: ['cpp'],
)

# Fully static runtime so the DLL drops into the jubeat folder alone.
add_project_link_arguments('-static', language: ['c', 'cpp'])

dlltool = find_program(meson.get_external_property('dlltool', 'dlltool'))

def_libs = []
foreach dll : ['avs2-core', 'gftools', 'layout']
    def_libs += custom_target(dll + '_implib',
        input: 'src' / (dll + '.def'),
        output: 'lib' + dll + '.a',
        command: [dlltool,
            '--input-def', '@INPUT@',
            '--output-lib', '@OUTPUT@',
            '-m', 'i386',
        ],
    )
endforeach

minhook = static_library('minhook',
    sources: [
        'third_party/minhook/src/buffer.c',
        'third_party/minhook/src/hde/hde32.c',
        'third_party/minhook/src/hde/hde64.c',
        'third_party/minhook/src/hook.c',
        'third_party/minhook/src/trampoline.c',
    ],
    include_directories: include_directories('third_party/minhook/include'),
    c_args: ['-w'],
)

python = find_program('python3', 'python')

# Decode overlay PNGs to raw RGBA blobs at build time so we can ship
# without lodepng. Each .h carries dimensions; the .rgba is #embed'd.
texture_data = []
foreach name : ['unheld', 'held']
    texture_data += custom_target(name + '_rgba',
        input: 'src/assets' / (name + '.png'),
        output: [name + '.rgba', name + '_dims.h'],
        command: [python, files('tools/png_to_rgba.py'), '@INPUT@', '@OUTDIR@'],
    )
endforeach

shared_library('tutorial_skip',
    sources: [
        'src/dllmain.cpp',
        'src/hook.cpp',
        'src/texture.cpp',
        texture_data,
    ],
    link_with: [minhook] + def_libs,
    link_args: ['-lgdi32', '-lopengl32'],
    cpp_args: ['--embed-dir=' + meson.current_build_dir()],
    name_prefix: '',
    vs_module_defs: 'src/tutorial_skip.def',
)
