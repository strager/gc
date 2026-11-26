#!/usr/bin/env bash

set -e
set -u
set -o pipefail

cc -g procmaps_test.c -o procmaps_test
./procmaps_test

cc -g testprog.c -o testprog

cc -g gc.c -pthread -fPIC -std=c23 -shared -o libgc.so -Wall -Wextra -fno-strict-aliasing -Wno-unused-function
LD_PRELOAD="${PWD}/libgc.so" figlet hello

imgui_src_dir=/home/strager/Projects/imgui/
imgui_build_dir=/home/strager/Projects/imgui/examples/example_sdl3_opengl3
c++ -o gcviz gcviz.cpp \
    -fsanitize=undefined \
    -DIMGUI_ENABLE_FREETYPE=1 -DIMGUI_ENABLE_STB_TRUETYPE=1 \
    -std=c++11 -I"${imgui_src_dir}" -I"${imgui_src_dir}/backends" \
    "${imgui_build_dir}/imgui.o" \
    "${imgui_build_dir}/imgui_draw.o" \
    "${imgui_build_dir}/imgui_tables.o" \
    "${imgui_build_dir}/imgui_widgets.o" \
    "${imgui_build_dir}/imgui_impl_sdl3.o" \
    "${imgui_build_dir}/imgui_impl_opengl3.o" \
    "${imgui_build_dir}/imgui_freetype.o" \
    -L/nix/store/270lhqjk19l9fbq4ya2j6cp39qkw111v-libglvnd-1.7.0/lib \
    -std=c++11 -I../.. -I../../backends -g -Wall -Wformat -DIMGUI_ENABLE_FREETYPE=1 -DIMGUI_ENABLE_STB_TRUETYPE=1 -I/nix/store/qc53ha5cdc551ffdl68872i3yc8lga7g-sdl3-3.4.10-dev/include -I/nix/store/wvj8547f271dds6p4jlil7jhh60mg170-freetype-2.14.3-dev/include/freetype2 -lGL -ldl -L/nix/store/ik3jsmgig8wmwhdpj0xcaa30vy56qdg3-sdl3-3.4.10-lib/lib -Wl,-rpath,/nix/store/ik3jsmgig8wmwhdpj0xcaa30vy56qdg3-sdl3-3.4.10-lib/lib -Wl,--enable-new-dtags -lSDL3 -L/nix/store/jwcfy3587dszy1lq5bpbb7fwff02qj1p-freetype-2.14.3/lib -lfreetype
