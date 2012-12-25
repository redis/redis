# build standalone mrbtest runner containing all *.rb tests

if(NOT CMAKE_CROSSCOMPILING)

  file(GLOB MRBTEST_SRC_RB "assert.rb" "t/*.rb")

  # generate a single rb file from all existing test *.rb
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.rbtmp"
    DEPENDS xpcat
    COMMAND xpcat -o "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.rbtmp" ${MRBTEST_SRC_RB}
    )

  # mruby compile and generate C byte array representation
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.ctmp"
    DEPENDS mrbc "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.rbtmp"
    COMMAND mrbc -Bmrbtest_irep -o"${CMAKE_CURRENT_BINARY_DIR}/mrbtest.ctmp"
                 "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.rbtmp"
    )

  # aggregate mruby's *.rb test files as a single C file
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.c"
    DEPENDS xpcat init_mrbtest.c "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.ctmp"
    COMMAND xpcat -o "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.c"
                     "${CMAKE_CURRENT_SOURCE_DIR}/init_mrbtest.c"
                     "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.ctmp"
    )

  add_executable(mrbtest
    EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/driver.c"
    "${CMAKE_CURRENT_BINARY_DIR}/mrbtest.c"
    )
  target_link_libraries(mrbtest libmruby_static ${MRUBY_LIBS})

  add_custom_target(test
    DEPENDS mrbtest
    COMMAND "${CMAKE_CURRENT_BINARY_DIR}/mrbtest"
    )

endif()

# vim: ts=2 sts=2 sw=2 et
