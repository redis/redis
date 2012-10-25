#include <stdio.h>
#ifdef __linux__
#include <gnu/libc-version.h>
#define GLIBC_VERSION "2.5"
#endif

int main (int argc, char* argv[])
{
    puts ("#ifndef __CONFIG_DEF_H__");
    puts ("#define __CONFIG_DEF_H__\n");

#ifdef __linux__
    const char *version = gnu_get_libc_version();
    if ( version && strcmp( GLIBC_VERSION, version ) == 0 ) {
        puts ("#define __LINUX_GLIBC25__");
        puts ("#include <asm/unistd.h>\n");
    }
#endif

    puts ("#endif /* __CONFIG_DEF_H__ */");
    return 0;
}

