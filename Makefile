.PHONY: all clean veryclean distclean wiretest modeltest utest help
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

# All driver layers in one archive; the final libsqrodbc.so (layer 3) links
# it plus the trampolines.
LIB     := $(ODIR)/libsqrodbc.a
SRC     := wire.cxx diag.cxx model.cxx catalog.cxx
OBJ     := $(addprefix $(ODIR)/,$(SRC:.cxx=.o))
HDRS    := $(wildcard src/*.hxx)

WIRE_TEST  := $(ODIR)/wire_test
MODEL_TEST := $(ODIR)/model_test

# The sqrd the tests drive (built by the sibling sqr project).
SQRD ?= ../sqr/obj_intel_release/sqrd

all: $(LIB) $(WIRE_TEST) $(MODEL_TEST)

$(ODIR):
	mkdir -p $@

$(ODIR)/%.o: src/%.cxx $(HDRS) | $(ODIR)
	$(CXX) -c $(CXX_OPTS) -o $@ $<

$(LIB): $(OBJ)
	@rm -f $@
	$(AR) rcs $@ $(OBJ)

$(ODIR)/%_test: test/%_test.cxx test/testutil.hxx $(HDRS) $(LIB) | $(ODIR)
	$(CXX) $(CXX_OPTS) -o $@ $< $(LIB) $(CXX_LOPTS)

# Both test programs spawn a real sqrd (scratch db, ephemeral port).
# wiretest doubles as protocol conformance; modeltest covers the object
# model (bindings, conversions, transactions, catalogue sets).
wiretest: $(WIRE_TEST)
	$(WIRE_TEST) $(SQRD)

modeltest: $(MODEL_TEST)
	$(MODEL_TEST) $(SQRD)

utest: wiretest modeltest

clean:
	@rm -vf $(ODIR)/*.o $(LIB) $(WIRE_TEST) $(MODEL_TEST) *~

veryclean: clean
	@rm -vfr obj_*

distclean: veryclean

help:
	@echo "Targets : all, utest (= wiretest + modeltest), clean, veryclean, distclean"
	@echo "Options : CXX=icpx|g++ (default icpx)  debug=1  SQRD=<path to sqrd>"
	@echo "ODIR    = $(ODIR)"
	@echo "CXX_OPTS= $(CXX_OPTS)"
