#!/bin/sh

objroot=$1

cat <<EOF
#ifndef JEMALLOC_H_
#define	JEMALLOC_H_
#ifdef __cplusplus
extern "C" {
#endif

EOF

for hdr in jemalloc_defs.h jemalloc_rename.h jemalloc_macros.h \
           jemalloc_protos.h jemalloc_typedefs.h jemalloc_mangle.h ; do
  cat "${objroot}include/jemalloc/${hdr}" \
      | grep -v 'Generated from .* by configure\.' \
      | sed -e 's/^#define /#define	/g' \
      | sed -e 's/ $//g'
  echo
done

cat <<EOF
#ifdef __cplusplus
}
#endif
#endif /* JEMALLOC_H_ */
EOF
