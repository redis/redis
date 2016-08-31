#!/bin/sh

usage() {
	cat <<EOF
Usage:
  @BINDIR@/jemalloc-config <option>
Options:
  --help | -h  : Print usage.
  --version    : Print jemalloc version.
  --revision   : Print shared library revision number.
  --config     : Print configure options used to build jemalloc.
  --prefix     : Print installation directory prefix.
  --bindir     : Print binary installation directory.
  --datadir    : Print data installation directory.
  --includedir : Print include installation directory.
  --libdir     : Print library installation directory.
  --mandir     : Print manual page installation directory.
  --cc         : Print compiler used to build jemalloc.
  --cflags     : Print compiler flags used to build jemalloc.
  --cppflags   : Print preprocessor flags used to build jemalloc.
  --ldflags    : Print library flags used to build jemalloc.
  --libs       : Print libraries jemalloc was linked against.
EOF
}

prefix="@prefix@"
exec_prefix="@exec_prefix@"

case "$1" in
--help | -h)
	usage
	exit 0
	;;
--version)
	echo "@jemalloc_version@"
	;;
--revision)
	echo "@rev@"
	;;
--config)
	echo "@CONFIG@"
	;;
--prefix)
	echo "@PREFIX@"
	;;
--bindir)
	echo "@BINDIR@"
	;;
--datadir)
	echo "@DATADIR@"
	;;
--includedir)
	echo "@INCLUDEDIR@"
	;;
--libdir)
	echo "@LIBDIR@"
	;;
--mandir)
	echo "@MANDIR@"
	;;
--cc)
	echo "@CC@"
	;;
--cflags)
	echo "@CFLAGS@"
	;;
--cppflags)
	echo "@CPPFLAGS@"
	;;
--ldflags)
	echo "@LDFLAGS@ @EXTRA_LDFLAGS@"
	;;
--libs)
	echo "@LIBS@"
	;;
*)
	usage
	exit 1
esac
