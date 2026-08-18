# Compiler options: g++ (secondary; GCC 16 at /opt/gcc-16.1.0 — on PATH in
# a login shell).
CXXNAME  := g++
CXX_BASE := -std=c++23 -Wall -Wextra -fPIC
CXX_LOPTS :=
# Extra link options for the shared driver: self-contained C++ runtime.
SO_LOPTS  := -static-libstdc++ -static-libgcc

ifdef debug
  CXX_BUILD := -g -O0
  ODIR      := obj_gxx_debug
else
  CXX_BUILD := -O2
  ODIR      := obj_gxx_release
endif
