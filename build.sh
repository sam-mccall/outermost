#!/bin/bash
set -e -x
clang++ --std=c++1z -o oterm -lutil -lX11 -Wno-switch -O3 *.cc
