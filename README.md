# sqr_odbc — ODBC driver for the sqr database server

A C++23 ODBC driver (ODBC 3.8 Core, ANSI) connecting ODBC applications —
first target LibreOffice — to a sqr database served by `sqrd`.

## Build and test

```
make                # CXX=icpx (default) or CXX=g++; debug=1 opt-in
make utest          # unit tests: wire (42) + model (72) + entry points (81)
make isqltest       # functional test: a full session through unixODBC's isql
```

The test programs spawn a real `sqrd` (default
`../sqr/obj_intel_release/sqrd`, override with `SQRD=`) on a scratch
database and an ephemeral port.

## Install and register

```
make install        # copies libsqrodbc.so to ~/.local/lib/sqrodbc/
make install-dsn    # registers the [sqr] data source in ~/.odbc.ini
```

Neither needs root. The data source points at the installed library by
full path, so no `odbcinst.ini` entry is required; edit `~/.odbc.ini` to
change `SERVER`/`PORT` (default 127.0.0.1:7477). The database is
whichever one the sqrd instance you connect to is serving.

Connection-string keys (for `SQLDriverConnect` users such as `isql -k`):
`DSN`, `SERVER`, `PORT`, and `DRIVER` as either a registered name or the
.so path. Explicit keys beat the DSN entry, which beats the defaults.

## Quick check

With a server running (`sqrd <db-dir> 7477`):

```
isql -v sqr
SQL> SELECT * FROM some_table;
```

## LibreOffice Base

1. Start sqrd on the database: `sqrd <db-dir> 7477`.
2. Base: *File → New → Database → Connect to an existing database → ODBC*,
   pick `sqr`, no user name or password, finish and save the .odb.
3. Tables appear under *Tables*; rows can be browsed and — for tables with
   a unique index — edited. *Tools → SQL* passes statements straight
   through for anything in sqr's SQL subset.

Calc can then pull the data via *Insert → Sheet from file* alternatives or
the registered data source (View → Data Sources, F4), or keep using the
sqr Calc client in `../sqr/calc` for the push/pull workflow.

## Layers

See `INFO.md`: wire client (`sqrw`), object model (`sqro`), extern "C"
trampolines packaged as a self-contained `libsqrodbc.so` (only
libodbcinst/libc/libm dynamic). The wire protocol is defined in
`../sqr/reports/DESIGN-wire-protocol.md`.
