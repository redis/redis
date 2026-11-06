#!/bin/sh
#
# Generate private_symbols[_jet].awk.
#
# Usage: private_symbols.sh <sym_prefix> <sym>*
#
# <sym_prefix> is typically "" or "_".

sym_prefix=$1
shift

cat <<EOF
#!/usr/bin/env awk -f

BEGIN {
  sym_prefix = "${sym_prefix}"
  split("\\
EOF

for public_sym in "$@" ; do
  cat <<EOF
        ${sym_prefix}${public_sym} \\
EOF
done

cat <<"EOF"
        ", exported_symbol_names)
  # Store exported symbol names as keys in exported_symbols.
  for (i in exported_symbol_names) {
    exported_symbols[exported_symbol_names[i]] = 1
  }
}

# Process 'nm -a <c_source.o>' output.
#
# Handle lines like:
#   0000000000000008 D opt_junk
#   0000000000007574 T malloc_initialized
(NF == 3 && $2 ~ /^[ABCDGRSTVW]$/ && !($3 in exported_symbols) && $3 ~ /^[A-Za-z0-9_]+$/) {
  print substr($3, 1+length(sym_prefix), length($3)-length(sym_prefix))
}

# Process 'dumpbin /SYMBOLS <c_source.obj>' output.
#
# Handle lines like:
#   353 00008098 SECT4  notype       External     | opt_junk
#   3F1 00000000 SECT7  notype ()    External     | malloc_initialized
($3 ~ /^SECT[0-9]+/ && $(NF-2) == "External" && !($NF in exported_symbols)) {
  print $NF
}
EOF
