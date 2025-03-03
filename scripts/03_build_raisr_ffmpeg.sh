#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 Intel Corporation

# This script is used to build raisr and ffmpeg

# Usage: 03_build_raisr_ffmpeg.sh /xxx/raisr/Video-Super-Resolution-Library

set -eo pipefail

SCRIPT_DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")")"
REPOSITORY_DIR="$(readlink -f "${SCRIPT_DIR}/../")"
. "${SCRIPT_DIR}/common.sh"

log_info Starting script execution "${BASH_SOURCE[0]}"
raisr_path=$1

if [ -z "$raisr_path" ];then
    echo "Usage: 03_build_raisr_ffmpeg.sh /xxx/raisr/Video-Super-Resolution-Library"
    exit 1
fi

# set IPP, x264 and x265 env
. /opt/intel/oneapi/ipp/latest/env/vars.sh
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH"
export C_INCLUDE_PATH="/opt/intel/oneapi/ipp/latest/include/ipp"

# build raisr
pushd "${raisr_path}"
sudo -E ./build.sh
popd

# TO-DO: Remove patch apply from bellow lines:
# Apply a temporary patch - this will be removed after version of FFmpeg gets updated
patch -d "${raisr_path}/../ffmpeg" -p1 -i <(cat "${raisr_path}/scripts/patch/ffmpeg/"*.patch)

# build ffmpeg
pushd "${raisr_path}/../ffmpeg"
cp "${raisr_path}/ffmpeg/vf_raisr.c" libavfilter/

./configure \
    --disable-debug \
    --disable-doc \
    --enable-libipp \
    --enable-gpl \
    --enable-libx264 \
    --enable-libx265 \
    --extra-libs='-lraisr -lstdc++ -lippcore -lippvm -lipps -lippi' \
    --extra-cflags='-fopenmp -I/opt/intel/oneapi/ipp/latest/include/ipp' \
    --extra-ldflags='-fopenmp' \
    --enable-cross-compile
make clean
make -j"$(nproc)"
sudo -E make install

# copy filters to ffmpeg directory
cp -r "${raisr_path}/filters"* .
popd

log_info "\tTo run the library you can do the following:"
log_info
log_info "\t\tcd raisr/ffmpeg"
log_info "\t\t./ffmpeg -y -i /input_files/input.mp4 -vf raisr=threadcount=20 -pix_fmt yuv420p /output_files/out.yuv"
log_info
log_info And you can see more use cases in README file of Video-Super-Resolution-Library.
log_info "\tNotice: If you get \"ffmpeg: error while loading shared libraries\", try first doing:"
log_info "\t\texport LD_LIBRARY_PATH=\"/opt/intel/oneapi/ipp/latest/lib/intel64:/usr/local/lib:${LD_LIBRARY_PATH}\""

log_info "Finished script execution \"${BASH_SOURCE[0]}\""
