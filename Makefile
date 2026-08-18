.PHONY: all clean veryclean distclean wiretest help
.SUFFIXES:
.DEFAULT_GOAL := all

# sqr_odbc — ODBC driver for the sqr database server (sqrd).
#
# Layered build (bottom up):
#   1. libsqrwire.a — the ODBC-free wire-protocol client (src/wire.*).
#   2. object model (Environment/Connection/Statement)     [to come]
#   3. extern "C" SQL* trampolines -> libsqrodbc.so        [to come]
#
# Compiler selection mirrors the Fortran projects' F= convention:
# CXX=icpx (default) or CXX=g++ (GCC 16), options in coptions_<tag>.mk,
# debug=1 for a debug build.

# CXX is a make builtin (default g++), so ?= would never fire: override the
# builtin default only, leaving an explicit CXX=g++ on the command line alone.
ifeq ($(origin CXX),default)
  CXX := icpx
endif
CXXTAG := $(subst +,x,$(CXX))
VALID_CXXTAG := icpx gxx
ifeq ($(filter $(CXXTAG),$(VALID_CXXTAG)),)
  $(error Unknown C++ compiler 'CXX=$(CXX)' -- choose icpx or g++)
endif

include coptions_$(CXXTAG).mk

CXX_OPTS := $(CXX_BASE) $(CXX_BUILD) -Isrc

WIRE_LIB := $(ODIR)/libsqrwire.a
WIRE_OBJ := $(ODIR)/wire.o
TEST_BIN := $(ODIR)/wire_test

# The sqrd the conformance test drives (built by the sibling sqr project).
SQRD ?= ../sqr/obj_intel_release/sqrd

all: $(WIRE_LIB) $(TEST_BIN)

$(ODIR):
	mkdir -p $@

$(WIRE_OBJ): src/wire.cxx src/wire.hxx | $(ODIR)
	$(CXX) -c $(CXX_OPTS) -o $@ $<

$(WIRE_LIB): $(WIRE_OBJ)
	@rm -f $@
	$(AR) rcs $@ $(WIRE_OBJ)

$(TEST_BIN): test/wire_test.cxx src/wire.hxx $(WIRE_LIB) | $(ODIR)
	$(CXX) $(CXX_OPTS) -o $@ $< $(WIRE_LIB) $(CXX_LOPTS)

# Conformance test: spawns a real sqrd (scratch db, ephemeral port) and
# checks the whole protocol surface, including binary-cell exactness.
wiretest: $(TEST_BIN)
	$(TEST_BIN) $(SQRD)

clean:
	@rm -vf $(ODIR)/*.o $(WIRE_LIB) $(TEST_BIN) *~

veryclean: clean
	@rm -vfr obj_*

distclean: veryclean

help:
	@echo "Targets : all, wiretest, clean, veryclean, distclean"
	@echo "Options : CXX=icpx|g++ (default icpx)  debug=1  SQRD=<path to sqrd>"
	@echo "ODIR    = $(ODIR)"
	@echo "CXX_OPTS= $(CXX_OPTS)"
