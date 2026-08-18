.PHONY: all clean veryclean distclean wiretest modeltest odbctest utest help
.SUFFIXES:
.DEFAULT_GOAL := all

# sqr_odbc — ODBC driver for the sqr database server (sqrd).
#
# Layered build (bottom up):
#   1. wire-protocol client (src/wire.*)
#   2. object model (Environment/Connection/Statement)
#   3. extern "C" SQL* trampolines (src/odbc.cxx) -> libsqrodbc.so
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

# Layers 1+2 in one archive (what the test programs link statically); the
# shared driver adds the trampolines and exports only SQL* (version script).
LIB     := $(ODIR)/libsqrodbc.a
SO      := $(ODIR)/libsqrodbc.so
SRC     := wire.cxx diag.cxx model.cxx catalog.cxx
OBJ     := $(addprefix $(ODIR)/,$(SRC:.cxx=.o))
HDRS    := $(wildcard src/*.hxx)

WIRE_TEST  := $(ODIR)/wire_test
MODEL_TEST := $(ODIR)/model_test
ODBC_TEST  := $(ODIR)/odbc_test

# The sqrd the tests drive (built by the sibling sqr project).
SQRD ?= ../sqr/obj_intel_release/sqrd

all: $(LIB) $(SO) $(WIRE_TEST) $(MODEL_TEST) $(ODBC_TEST)

$(ODIR):
	mkdir -p $@

$(ODIR)/%.o: src/%.cxx $(HDRS) | $(ODIR)
	$(CXX) -c $(CXX_OPTS) -o $@ $<

$(LIB): $(OBJ)
	@rm -f $@
	$(AR) rcs $@ $(OBJ)

$(ODIR)/%_test: test/%_test.cxx test/testutil.hxx $(HDRS) $(LIB) | $(ODIR)
	$(CXX) $(CXX_OPTS) -o $@ $< $(LIB) $(CXX_LOPTS)

# The shared driver: trampolines + the archive, SQL* exported, C++ runtime
# linked in, libodbcinst (DSN lookup) left dynamic.
$(SO): $(ODIR)/odbc.o $(LIB) src/sqrodbc.map
	$(CXX) -shared -o $@ $(ODIR)/odbc.o $(LIB) \
	  -Wl,-soname,libsqrodbc.so -Wl,--version-script=src/sqrodbc.map \
	  $(SO_LOPTS) -lodbcinst

# odbc_test drives the driver through the .so and the C API — exactly the
# surface the driver manager loads.
$(ODBC_TEST): test/odbc_test.cxx test/testutil.hxx $(SO) | $(ODIR)
	$(CXX) $(CXX_OPTS) -o $@ $< -L$(ODIR) -lsqrodbc -Wl,-rpath,$(CURDIR)/$(ODIR) $(CXX_LOPTS)

# All test programs spawn a real sqrd (scratch db, ephemeral port).
# wiretest doubles as protocol conformance; modeltest covers the object
# model (bindings, conversions, transactions, catalogue sets); odbctest
# covers the SQL* entry points through the shared library.
wiretest: $(WIRE_TEST)
	$(WIRE_TEST) $(SQRD)

modeltest: $(MODEL_TEST)
	$(MODEL_TEST) $(SQRD)

odbctest: $(ODBC_TEST)
	$(ODBC_TEST) $(SQRD)

utest: wiretest modeltest odbctest

clean:
	@rm -vf $(ODIR)/*.o $(LIB) $(SO) $(WIRE_TEST) $(MODEL_TEST) $(ODBC_TEST) *~

veryclean: clean
	@rm -vfr obj_*

distclean: veryclean

help:
	@echo "Targets : all, utest (= wiretest + modeltest + odbctest), clean, veryclean, distclean"
	@echo "Options : CXX=icpx|g++ (default icpx)  debug=1  SQRD=<path to sqrd>"
	@echo "ODIR    = $(ODIR)"
	@echo "CXX_OPTS= $(CXX_OPTS)"
