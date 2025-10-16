/*
 * Generates a Unicode test for xxhsum without using Unicode in the source files.
 *
 * Copyright (C) 2020 Devin Hussey (easyaspi314)
 *
 * BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Certain terminals don't properly handle UTF-8 (i.e. rxvt and command prompt
 * in the default codepage), and that can cause issues when editing text.
 *
 * We use this C file to generate a file with a Unicode filename, a file with
 * a checksum of said file, and both a Windows batch script and a Unix shell
 * script to test the file.
 */

#define _CRT_SECURE_NO_WARNINGS /* Silence warnings on MSVC */
#include <stdio.h>

/* Use a Japanese filename, something that can't be cheated with ANSI.
 * yuniko-do.unicode (literally unicode.unicode) */

/* Use raw hex values to ensure that the output is well-formed UTF-8. It is also more C90 compliant. */
static const char FILENAME[] = {
    (char)0xe3, (char)0x83, (char)0xa6,  /* U+30e6: Katakana letter yu */
    (char)0xe3, (char)0x83, (char)0x8b,  /* U+30cb: Katakana letter ni */
    (char)0xe3, (char)0x82, (char)0xb3,  /* U+30b3: Katakana letter ko */
    (char)0xe3, (char)0x83, (char)0xbc,  /* U+30fc: Katakana-Hiragana prolonged sound mark (dash) */
    (char)0xe3, (char)0x83, (char)0x89,  /* U+30c9: Katakana letter do */
    '.','u','n','i','c','o','d','e','\0' /* ".unicode" (so we can glob in make clean and .gitignore) */
};

#ifdef _WIN32
/* The same text as above, but encoded in Windows UTF-16. */
static const wchar_t WFILENAME[] = { 0x30e6, 0x30cb, 0x30b3, 0x30fc, 0x30c9, L'.', L'u', L'n', L'i', L'c', L'o', L'd', L'e', L'\0' };
#endif

int main(void)
{
    FILE *f, *script, *checksum;

    /* Create our Unicode file. Use _wfopen on Windows as fopen doesn't support Unicode filenames. */
#ifdef _WIN32
    if (!(f = _wfopen(WFILENAME, L"wb"))) return 1;
#else
    if (!(f = fopen(FILENAME, "wb"))) return 1;
#endif
    fprintf(f, "test\n");
    fclose(f);

    /* XXH64 checksum file with the precalculated checksum for said file. */
    if (!(checksum = fopen("unicode_test.xxh64", "wb")))
        return 1;
    fprintf(checksum, "2d7f1808da1fa63c  %s\n", FILENAME);
    fclose(checksum);


    /* Create two scripts for both Windows and Unix. */

    /* Generate a Windows batch script. Always insert CRLF manually. */
    if (!(script = fopen("unicode_test.bat", "wb")))
        return 1;

    /* Disable echoing the commands. We do that ourselves the naive way. */
    fprintf(script, "@echo off\r\n");

    /* Change to codepage 65001 to enable UTF-8 support. */
    fprintf(script, "chcp 65001 >NUL 2>&1\r\n");

    /* First test a Unicode filename */
    fprintf(script, "echo Testing filename provided on command line...\r\n");
    fprintf(script, "echo xxhsum.exe \"%s\"\r\n", FILENAME);
    fprintf(script, "xxhsum.exe \"%s\"\r\n", FILENAME);

    /* Bail on error */
    fprintf(script, "if %%ERRORLEVEL%% neq 0 (\r\n");
    fprintf(script, "    exit /B %%ERRORLEVEL%%\r\n");
    fprintf(script, ")\r\n");

    /* Then test a checksum file. */
    fprintf(script, "echo Testing a checksum file...\r\n");
    fprintf(script, "echo xxhsum.exe -c unicode_test.xxh64\r\n");
    fprintf(script, "xxhsum.exe -c unicode_test.xxh64\r\n");

    fprintf(script, "exit /B %%ERRORLEVEL%%\r\n");

    fclose(script);

    /* Generate a Unix shell script */
    if (!(script = fopen("unicode_test.sh", "wb")))
        return 1;

    fprintf(script, "#!/bin/sh\n");
    /*
     * Some versions of MSYS, MinGW and Cygwin do not support UTF-8, and the ones that
     * don't may error with something like this:
     *
     *    Error: Could not open '<mojibake>.unicode': No such file or directory.
     *
     * which is an internal error that happens when it tries to convert MinGW/Cygwin
     * paths to Windows paths.
     *
     * In that case, we bail to cmd.exe and the batch script, which supports UTF-8
     * on Windows 7 and later.
     */
    fprintf(script, "case $(uname) in\n");
    /* MinGW/MSYS converts /c to C:\ unless you have a double slash,
     * Cygwin does not. */
    fprintf(script, "    *CYGWIN*)\n");
    fprintf(script, "        exec cmd.exe /c unicode_test.bat\n");
    fprintf(script, "        ;;\n");
    fprintf(script, "    *MINGW*|*MSYS*)\n");
    fprintf(script, "        exec cmd.exe //c unicode_test.bat\n");
    fprintf(script, "        ;;\n");
    fprintf(script, "esac\n");

    /* First test a Unicode filename */
    fprintf(script, "echo Testing filename provided on command line...\n");
    fprintf(script, "echo './xxhsum \"%s\" || exit $?'\n", FILENAME);
    fprintf(script, "./xxhsum \"%s\" || exit $?\n", FILENAME);

    /* Then test a checksum file. */
    fprintf(script, "echo Testing a checksum file...\n");
    fprintf(script, "echo './xxhsum -c unicode_test.xxh64 || exit $?'\n");
    fprintf(script, "./xxhsum -c unicode_test.xxh64 || exit $?\n");

    fclose(script);

    return 0;
}
