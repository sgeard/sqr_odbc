# Compiler options: Intel icpx (primary).  icpx auto-selects the GCC 16
# toolchain at /opt/gcc-16.1.0, giving the full C++23 standard library
# (std::expected, format, print, flat_map, round-trip to_chars).
CXXNAME  := icpx
CXX_BASE := -std=c++23 -Wall -Wextra -fPIC
CXX_LOPTS := -static-intel
# Extra link options for the shared driver: self-contained C++ runtime.
SO_LOPTS  := -static-intel -static-libstdc++ -static-libgcc

ifdef debug
  CXX_BUILD := -g -O0
  ODIR      := obj_intel_debug
else
  CXX_BUILD := -O2
  ODIR      := obj_intel_release
endif
