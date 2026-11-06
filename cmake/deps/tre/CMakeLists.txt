add_library(tre STATIC
    ${REDIS_ROOT}/deps/tre/lib/regcomp.c
    ${REDIS_ROOT}/deps/tre/lib/regerror.c
    ${REDIS_ROOT}/deps/tre/lib/regexec.c
    ${REDIS_ROOT}/deps/tre/lib/tre-ast.c
    ${REDIS_ROOT}/deps/tre/lib/tre-compile.c
    ${REDIS_ROOT}/deps/tre/lib/tre-filter.c
    ${REDIS_ROOT}/deps/tre/lib/tre-match-backtrack.c
    ${REDIS_ROOT}/deps/tre/lib/tre-match-parallel.c
    ${REDIS_ROOT}/deps/tre/lib/tre-mem.c
    ${REDIS_ROOT}/deps/tre/lib/tre-parse.c
    ${REDIS_ROOT}/deps/tre/lib/tre-stack.c
    ${REDIS_ROOT}/deps/tre/lib/xmalloc.c
)

target_include_directories(tre PUBLIC
    ${REDIS_ROOT}/deps/tre/lib
    ${REDIS_ROOT}/deps/tre/local_includes
)

target_compile_definitions(tre PRIVATE TRE_REGEX_T_FIELD=value)

set_target_properties(tre PROPERTIES FOLDER "deps")
