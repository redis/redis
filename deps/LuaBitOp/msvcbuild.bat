@rem Script to build Lua BitOp with MSVC.

@rem First change the paths to your Lua installation below.
@rem Then open a "Visual Studio .NET Command Prompt", cd to this directory
@rem and run this script. Afterwards copy the resulting bit.dll to
@rem the directory where lua.exe is installed.

@if not defined INCLUDE goto :FAIL

@setlocal
@rem Path to the Lua includes and the library file for the Lua DLL:
@set LUA_INC=-I ..
@set LUA_LIB=..\lua51.lib

@set MYCOMPILE=cl /nologo /MD /O2 /W3 /c %LUA_INC%
@set MYLINK=link /nologo
@set MYMT=mt /nologo

%MYCOMPILE% bit.c
%MYLINK% /DLL /export:luaopen_bit /out:bit.dll bit.obj %LUA_LIB%
if exist bit.dll.manifest^
  %MYMT% -manifest bit.dll.manifest -outputresource:bit.dll;2

del *.obj *.exp *.manifest

@goto :END
:FAIL
@echo You must open a "Visual Studio .NET Command Prompt" to run this script
:END
