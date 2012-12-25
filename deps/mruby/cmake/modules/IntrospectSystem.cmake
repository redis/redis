# system capabilities checking

# initial system defaults
if(CMAKE_COMPILER_IS_GNUCC)
  set(MRUBY_DEFAULT_CFLAGS "-std=gnu99 -Wall -Werror-implicit-function-declaration -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign -Wno-missing-field-initializers -Wformat=2 -Wstrict-aliasing=2 -Wdisabled-optimization -Wpointer-arith -Wdeclaration-after-statement -Wbad-function-cast -Wcast-align -Wredundant-decls")
  set(CMAKE_C_FLAGS "${MRUBY_DEFAULT_CFLAGS}")
  set(CMAKE_C_FLAGS_DEBUG "-O3 -ggdb")
  set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -g")
  set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

  set(MRUBY_LIBS m)
else()
  if(MSVC)
    # TODO default MSVC flags
    add_definitions(
      -D_CRT_SECURE_NO_WARNINGS
      -wd4018  # suppress 'signed/unsigned mismatch'
      )
  endif()
endif()

add_definitions(-DDISABLE_GEMS)

if(MSVC)
  add_definitions(
    -DRUBY_EXPORT   # required by oniguruma.h
    )
endif()


# include helpers
include(CheckIncludeFile)
include(CheckSymbolExists)

# header checks
CHECK_INCLUDE_FILE(string.h HAVE_STRING_H)
if(HAVE_STRING_H)
  add_definitions(-DHAVE_STRING_H)
endif()

CHECK_INCLUDE_FILE(float.h HAVE_FLOAT_H)
if(HAVE_FLOAT_H)
  add_definitions(-DHAVE_FLOAT_H)
endif()


# symbol checks
CHECK_SYMBOL_EXISTS(gettimeofday sys/time.h HAVE_GETTIMEOFDAY)
if(NOT HAVE_GETTIMEOFDAY)
  add_definitions(-DNO_GETTIMEOFDAY)
endif()

# vim: ts=2 sts=2 sw=2 et
