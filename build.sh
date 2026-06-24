#!/bin/bash
set -euo pipefail

directories=("./build" "./resource/download" "./resource/generate" "./resource/r_color")

for directory in "${directories[@]}"; do
    if [ ! -d "$directory" ]; then
        mkdir -p "$directory"
    fi
done

build_type="Release"
cmake_extra_args=()
if [[ "${2:-}" == "sanitize" || "${SANITIZE:-0}" == "1" ]]; then
    build_type="RelWithDebInfo"
    sanitize_flags="-fsanitize=address -fno-omit-frame-pointer -g"
    cmake_extra_args+=("-DCMAKE_CXX_FLAGS=${sanitize_flags}")
    cmake_extra_args+=("-DCMAKE_EXE_LINKER_FLAGS=${sanitize_flags}")
    cmake_extra_args+=("-DCMAKE_SHARED_LINKER_FLAGS=${sanitize_flags}")
fi

cmake -S . -B ./build -DCMAKE_BUILD_TYPE="${build_type}" -DMODE="${1:-}" "${cmake_extra_args[@]}"
cmake --build ./build -j"$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))"