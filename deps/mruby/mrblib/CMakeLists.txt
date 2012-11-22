# transform mruby's standard lib into a C library

file(GLOB MRBLIB_SRC_RB "*.rb")

if(CMAKE_CROSSCOMPILING)
  # create native tools and `mrblib.ctmp` required to build `mrblib.c`
  include(ExternalProject)
  ExternalProject_Add(mruby-native
    DOWNLOAD_COMMAND ""
    SOURCE_DIR "${CMAKE_SOURCE_DIR}"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" "${CMAKE_SOURCE_DIR}"
    INSTALL_COMMAND ""
    BINARY_DIR "${CMAKE_BINARY_DIR}/native"
    )

  # aggregate mruby's standard library as a single C file
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mrblib.c"
    DEPENDS mruby-native init_mrblib.c "${CMAKE_BINARY_DIR}/native/mrblib/mrblib.ctmp"
    COMMAND "${CMAKE_BINARY_DIR}/native/tools/xpcat/xpcat"
            -o "${CMAKE_CURRENT_BINARY_DIR}/mrblib.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/init_mrblib.c"
            "${CMAKE_BINARY_DIR}/native/mrblib/mrblib.ctmp"
    )
else()
  # generate a single rb file from all existing ones
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mrblib.rbtmp"
    DEPENDS xpcat
    COMMAND xpcat -o "${CMAKE_CURRENT_BINARY_DIR}/mrblib.rbtmp" ${MRBLIB_SRC_RB}
    )

  # mruby compile and generate C byte array representation
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mrblib.ctmp"
    DEPENDS mrbc "${CMAKE_CURRENT_BINARY_DIR}/mrblib.rbtmp"
    COMMAND mrbc -Bmrblib_irep -o"${CMAKE_CURRENT_BINARY_DIR}/mrblib.ctmp"
                 "${CMAKE_CURRENT_BINARY_DIR}/mrblib.rbtmp"
    )

  # aggregate mruby's standard library as a single C file
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mrblib.c"
    DEPENDS xpcat init_mrblib.c "${CMAKE_CURRENT_BINARY_DIR}/mrblib.ctmp"
    COMMAND xpcat -o "${CMAKE_CURRENT_BINARY_DIR}/mrblib.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/init_mrblib.c"
                     "${CMAKE_CURRENT_BINARY_DIR}/mrblib.ctmp"
    )
endif()


# generate final static libmruby archive library
add_library(libmruby_static STATIC
  mrblib.c
  $<TARGET_OBJECTS:mruby_object>
  )
set_target_properties(libmruby_static PROPERTIES OUTPUT_NAME mruby)

install(TARGETS libmruby_static
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  )

# vim: ts=2 sts=2 sw=2 et
