#!/usr/bin/env bash
# 从仓库根目录运行:bash tests/host/run.sh
set -e
g++ -std=gnu++20 -g -fsanitize=address,undefined -O1 \
    -o /tmp/m4a_demuxer_test \
    tests/host/m4a_demuxer_test.cc main/audio/demuxer/m4a_demuxer.cc
/tmp/m4a_demuxer_test
