#!/usr/bin/env perl

# Create test comparison data using a different UTF-8 implementation.

use strict;
use warnings;
use Text::Iconv;
use FileHandle;

# 0xD800 - 0xDFFF are used to encode supplementary codepoints
# 0x10000 - 0x10FFFF are supplementary codepoints
my (@codepoints) = (0 .. 0xD7FF, 0xE000 .. 0x10FFFF);

my ($utf32be) = pack("N*", @codepoints);
my $iconv = Text::Iconv->new("UTF-32BE", "UTF-8");
my ($utf8) = $iconv->convert($utf32be);
defined($utf8) or die "Unable create UTF-8 string\n";

my $fh = FileHandle->new();
$fh->open("utf8.dat", ">")
    or die "Unable to open utf8.dat: $!\n";
$fh->print($utf8)
    or die "Unable to write utf.dat\n";
$fh->close();

# vi:ai et sw=4 ts=4:
