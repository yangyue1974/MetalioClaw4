#!/usr/bin/env bash
# 从仓库根目录运行:bash tests/host/run.sh
set -e
g++ -std=gnu++20 -g -fsanitize=address,undefined -O1 \
    -o /tmp/m4a_demuxer_test \
    tests/host/m4a_demuxer_test.cc main/audio/demuxer/m4a_demuxer.cc
/tmp/m4a_demuxer_test

# bt_audio.cc 用 #include "IOExpander.hpp" 这种带引号的 include,会先找源文件所在目录,
# 所以把它拷到桩目录旁边编,桩才能盖住真头文件。
rm -rf /tmp/bt_audio_host && mkdir -p /tmp/bt_audio_host
cp tests/host/stubs/*.h tests/host/stubs/*.hpp /tmp/bt_audio_host/
cp -r tests/host/stubs/freertos /tmp/bt_audio_host/
cp main/boards/common/bt_audio.h main/boards/common/bt_audio.cc /tmp/bt_audio_host/
g++ -std=gnu++20 -g -fsanitize=address,undefined -O1 -I/tmp/bt_audio_host \
    -o /tmp/bt_audio_test \
    tests/host/bt_audio_test.cc /tmp/bt_audio_host/bt_audio.cc
/tmp/bt_audio_test
