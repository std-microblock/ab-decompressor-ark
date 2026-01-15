add_requires("lzham_codec", "lz4", "lzma")
set_languages("cxx23")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})

target("lzham-ab-decompressor")
    add_files("src/*.cc")
    add_packages("lzham_codec", "lz4", "lzma")