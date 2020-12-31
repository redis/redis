set(LUA_SRC
        src/lapi.c src/lcode.c src/ldebug.c src/ldo.c src/ldump.c src/lfunc.c
        src/lgc.c src/llex.c src/lmem.c
        src/lobject.c src/lopcodes.c src/lparser.c src/lstate.c src/lstring.c
        src/ltable.c src/ltm.c
        src/lundump.c src/lvm.c src/lzio.c src/strbuf.c src/fpconv.c
        src/lauxlib.c src/lbaselib.c src/ldblib.c src/liolib.c src/lmathlib.c
        src/loslib.c src/ltablib.c
        src/lstrlib.c src/loadlib.c src/linit.c src/lua_cjson.c
        src/lua_struct.c
        src/lua_cmsgpack.c
        src/lua_bit.c
        )

add_library(lua STATIC ${LUA_SRC})