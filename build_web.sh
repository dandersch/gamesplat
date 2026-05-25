#!/bin/bash

em++ -sINITIAL_MEMORY=671088640 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sFULL_ES3 -sUSE_SDL=3 \
    -I/usr/include/GL -I./third_party/imgui  -I./third_party/imgui/backends -Ithird_party \
    src/main.cpp third_party/third_party_impl.cpp third_party/imgui/*.cpp third_party/imgui/backends/imgui_impl_sdl3.cpp \
    --preload-file res/transition.wav --preload-file test/export_n01.ply \
    --preload-file test/colmap/images/ --preload-file test/colmap/sparse/0/ \
    --preload-file test/cyberpunk_guy.glb --preload-file test/priest.glb \
    -o build/index.html
