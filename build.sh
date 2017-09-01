#!/bin/bash
set -e -x
clang++ --std=c++14 -o oterm -lutil -lX11 term.cc
