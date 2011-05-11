@rem Script to test Lua BitOp.

@setlocal
@rem Path to the Lua executable:
@set LUA=lua

@echo off
for %%t in (bittest.lua nsievebits.lua md5test.lua) do (
  echo testing %%t
  %LUA% %%t
  if errorlevel 1^
    goto :FAIL
)
echo ****** ALL TESTS OK ******
goto :END

:FAIL
echo ****** TEST FAILED ******
:END
