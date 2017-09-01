#!/bin/bash
set -e -x
clang++ --std=c++1z -o oterm -lutil -lX11 term.cc
