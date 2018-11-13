#!/bin/bash
gcc -Wall -Wpointer-sign ffserver.c -I .  -lavformat -lavcodec -lavutil -lz -lm -lpthread -ldl -lx264 -llzma -lfdk-aac -lswresample
echo "Done"
