project(lualib)

include(CheckFunctionExists)

set(LUA_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/src")
set(LUA_SRCS
    ${LUA_SRC_DIR}/fpconv.c
    ${LUA_SRC_DIR}/lbaselib.c
    ${LUA_SRC_DIR}/lmathlib.c
    ${LUA_SRC_DIR}/lstring.c
    ${LUA_SRC_DIR}/lparser.c
    ${LUA_SRC_DIR}/ldo.c
    ${LUA_SRC_DIR}/lzio.c
    ${LUA_SRC_DIR}/lmem.c
    ${LUA_SRC_DIR}/strbuf.c
    ${LUA_SRC_DIR}/lstrlib.c
    ${LUA_SRC_DIR}/lundump.c
    ${LUA_SRC_DIR}/lua_cmsgpack.c
    ${LUA_SRC_DIR}/loslib.c
    ${LUA_SRC_DIR}/lua_struct.c
    ${LUA_SRC_DIR}/ldebug.c
    ${LUA_SRC_DIR}/lobject.c
    ${LUA_SRC_DIR}/ldump.c
    ${LUA_SRC_DIR}/lua_cjson.c
    ${LUA_SRC_DIR}/ldblib.c
    ${LUA_SRC_DIR}/ltm.c
    ${LUA_SRC_DIR}/ltable.c
    ${LUA_SRC_DIR}/lstate.c
    ${LUA_SRC_DIR}/lua_bit.c
    ${LUA_SRC_DIR}/lua.c
    ${LUA_SRC_DIR}/loadlib.c
    ${LUA_SRC_DIR}/lcode.c
    ${LUA_SRC_DIR}/lapi.c
    ${LUA_SRC_DIR}/lgc.c
    ${LUA_SRC_DIR}/lvm.c
    ${LUA_SRC_DIR}/lfunc.c
    ${LUA_SRC_DIR}/lauxlib.c
    ${LUA_SRC_DIR}/ltablib.c
    ${LUA_SRC_DIR}/linit.c
    ${LUA_SRC_DIR}/lopcodes.c
    ${LUA_SRC_DIR}/llex.c
    ${LUA_SRC_DIR}/liolib.c)

add_library(lualib STATIC "${LUA_SRCS}")
target_include_directories(lualib PUBLIC "${LUA_SRC_DIR}")
target_compile_definitions(lualib PRIVATE ENABLE_CJSON_GLOBAL)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

# Use mkstemp if available
check_function_exists(mkstemp HAVE_MKSTEMP)
if (HAVE_MKSTEMP)
    target_compile_definitions(lualib PRIVATE LUA_USE_MKSTEMP)
endif ()
unset(HAVE_MKSTEMP CACHE)
