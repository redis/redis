Introduction
============

TRE is a lightweight, robust, and efficient POSIX compliant regexp
matching library with some exciting features such as approximate
(fuzzy) matching.

The matching algorithm used in TRE uses linear worst-case time in
the length of the text being searched, and quadratic worst-case
time in the length of the used regular expression.

In other words, the time complexity of the algorithm is O(M^2N), where
M is the length of the regular expression and N is the length of the
text.  The used space is also quadratic on the length of the regex,
but does not depend on the searched string.  This quadratic behaviour
occurs only on pathological cases which are probably very rare in
practice.


Hacking
=======

Here's how to work with this code.

Prerequisites
-------------

You will need the following tools installed on your system:

  - autoconf
  - automake
  - gettext (including autopoint)
  - libtool
  - zip (optional)


Building
--------

First, prepare the tree.  Change to the root of the source directory
and run

    ./utils/autogen.sh

This will regenerate various things using the prerequisite tools so
that you end up with a buildable tree.

After this, you can run the configure script and build TRE as usual:

    ./configure
    make
    make check
    make install


Building a source code package
------------------------------

In a prepared tree, this command creates a source code tarball:

    ./configure && make dist

Alternatively, you can run

    ./utils/build-sources.sh

which builds the source code packages and puts them in the `dist`
subdirectory.  This script needs a working `zip` command.


Features
========

TRE is not just yet another regexp matcher. TRE has some features
which are not there in most free POSIX compatible implementations.
Most of these features are not present in non-free implementations
either, for that matter.

Approximate matching
--------------------

Approximate pattern matching allows matches to be approximate, that
is, allows the matches to be close to the searched pattern under some
measure of closeness.  TRE uses the edit-distance measure (also known
as the Levenshtein distance) where characters can be inserted,
deleted, or substituted in the searched text in order to get an exact
match.

Each insertion, deletion, or substitution adds the distance, or cost,
of the match.  TRE can report the matches which have a cost lower than
some given threshold value.  TRE can also be used to search for
matches with the lowest cost.

TRE includes a version of the agrep (approximate grep) command line
tool for approximate regexp matching in the style of grep. Unlike
other agrep implementations (like the one by Sun Wu and Udi Manber
from University of Arizona) TRE agrep allows full regexps of any
length, any number of errors, and non-uniform costs for insertion,
deletion and substitution.

Strict standard conformance
---------------------------

POSIX defines the behaviour of regexp functions precisely.  TRE
attempts to conform to these specifications as strictly as possible.
TRE always returns the correct matches for subpatterns, for example.
Very few other implementations do this correctly.  In fact, the only
other implementations besides TRE that I am aware of (free or not)
that get it right are Rx by Tom Lord, Regex++ by John Maddock, and the
AT&T ast regex by Glenn Fowler and Doug McIlroy.

The standard TRE tries to conform to is the IEEE Std 1003.1-2001, or
Open Group Base Specifications Issue 6, commonly referred to as
“POSIX”.  The relevant parts are the base specifications on regular
expressions (and the rationale) and the description of the `regcomp()`
API.

For an excellent survey on POSIX regexp matchers, see the testregex
pages by Glenn Fowler of AT&T Labs Research.

Predictable matching speed
--------------------------

Because of the matching algorithm used in TRE, the maximum time
consumed by any `regexec()` call is always directly proportional to
the length of the searched string. There is one exception: if back
references are used, the matching may take time that grows
exponentially with the length of the string.  This is because matching
back references is an NP complete problem, and almost certainly
requires exponential time to match in the worst case.

Predictable and modest memory consumption
-----------------------------------------

A `regexec()` call never allocates memory from the heap. TRE allocates
all the memory it needs during a `regcomp()` call, and some temporary
working space from the stack frame for the duration of the `regexec()`
call.  The amount of temporary space needed is constant during
matching and does not depend on the searched string. For regexps of
reasonable size TRE needs less than 50K of dynamically allocated
memory during the `regcomp()` call, less than 20K for the compiled
pattern buffer, and less than two kilobytes of temporary working space
from the stack frame during a `regexec()` call.  There is no time /
memory tradeoff. TRE is also small in code size; statically linking
with TRE increases the executable size less than 30K (gcc-3.2, x86,
GNU/Linux).

Wide character and multibyte character set support
--------------------------------------------------

TRE supports multibyte character sets.  This makes it possible to use
regexps seamlessly with, for example, Japanese locales.  TRE also
provides a wide character API.

Binary pattern and data support
-------------------------------

TRE provides APIs which allow binary zero characters both in regexps
and searched strings.  The standard API cannot be easily used to, for
example, search for printable words from binary data (although it is
possible with some hacking).  Searching for patterns which contain
binary zeroes embedded is not possible at all with the standard API.

Completely thread safe
----------------------

TRE is completely thread safe.  All the exported functions are
re-entrant, and a single compiled regexp object can be used
simultaneously in multiple contexts; e.g. in `main()` and a signal
handler, or in many threads of a multithreaded application.

Portable
--------

TRE is portable across multiple platforms.  Below is a table of
platforms and compilers used to develop and test TRE:

<table>
	<tr><th>Platform</th>				<th>Compiler</th></tr>
	<tr><td>FreeBSD 14.1</td>			<td>Clang 18</td></tr>
	<tr><td>Ubuntu 22.04</td>			<td>GCC 11</td></tr>
	<tr><td>macOS 14.6</td>				<td>Clang 14</td></tr>
	<tr><td>Windows 11</td>				<td>Microsoft Visual Studio 2022</td></tr>
</table>

TRE should compile without changes on most modern POSIX-like
platforms, and be easily portable to any platform with a hosted C
implementation.

Depending on the platform, you may need to install libutf8 to get
wide character and multibyte character set support.

Free
----

TRE is released under a license which is essentially the same as the
“2 clause” BSD-style license used in NetBSD.  See the file LICENSE for
details.

Roadmap
-------

There are currently two features, both related to collating elements,
missing from 100% POSIX compliance.  These are:

* Support for collating elements (e.g. `[[.\<X>.]]`, where `\<X>` is a
  collating element).  It is not possible to support multi-character
  collating elements portably, since POSIX does not define a way to
  determine whether a character sequence is a multi-character
  collating element or not.

* Support for equivalence classes, for example `[[=\<X>=]]`, where
  `\<X>` is a collating element.  An equivalence class matches any
  character which has the same primary collation weight as `\<X>`.
  Again, POSIX provides no portable mechanism for determining the
  primary collation weight of a collating element.

Note that other portable regexp implementations don't support
collating elements either.  The single exception is Regex++, which
comes with its own database for collating elements for different
locales.  Support for collating elements and equivalence classes has
not been widely requested and is not very high on the TODO list at the
moment.

These are other features I'm planning to implement real soon now:

* All the missing GNU extensions enabled in GNU regex, such as
  `[[:<:]]` and `[[:>:]]`.

* A `REG_SHORTEST` `regexec()` flag for returning the shortest match
  instead of the longest match.

* Perl-compatible syntax:
  * `[:^class:]`
    Matches anything but the characters in class.  Note that
    `[^[:class:]]` works already, this would be just a convenience
    shorthand.

  * `\A`
    Match only at beginning of string.

  * `\Z`
    Match only at end of string, or before newline at the end.

  * `\z`
    Match only at end of string.

  * `\l`
    Lowercase next char (think vi).

  * `\u`
    Uppercase next char (think vi).

  * `\L`
    Lowercase till `\E` (think vi).

  * `\U`
    Uppercase till `\E` (think vi).

  * `(?=pattern)`
    Zero-width positive look-ahead assertions.

  * `(?!pattern)`
    Zero-width negative look-ahead assertions.

  * `(?<=pattern)`
    Zero-width positive look-behind assertions.

  * `(?<!pattern)`
    Zero-width negative look-behind assertions.

Documentation especially for the nonstandard features of TRE, such as
approximate matching, is a work in progress (with “progress” loosely
defined...)  If you want to find an extension to use, reading the
`include/tre/tre.h` header might provide some additional hints if you
are comfortable with C source code.
