/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define ANSI_DR "\033[31;22m"
#define ANSI_BR "\033[31;1m"
#define ANSI_BB "\033[34;1m"
#define ANSI_GR "\033[30;1m"
#define ANSI_DW "\033[37;22m"
#define ANSI_BW "\033[37;1m"
#define ANSI_CL "\033[0m"
#define VER_LINE  ANSI_BW "Redis %s (%s/%d) %s bit\n"
#define MODE_LINE ANSI_DW "Running in %s mode\n"
#define PORT_LINE ANSI_DW "Port: %d\n"
#define PID_LINE  ANSI_DW "PID: %ld\n"
#define URL_LINE  ANSI_BB "http://redis.io\n"

char *ascii_logo_color = ANSI_DR
"                _._\n"
"           _.-``" ANSI_DW "__" ANSI_DR " ''-._\n"
"      _.-``    " ANSI_DW "`.  `_." ANSI_DR "  ''-._           " VER_LINE ANSI_DR
"  .-``" ANSI_DW " .-```.  ```\\/"ANSI_GR "    _.,_ " ANSI_DR "''-._\n"
" (    " ANSI_DW "'      ," ANSI_GR "       .-`  | `," ANSI_DR "    )     " MODE_LINE ANSI_DR
" |`-._" ANSI_DW "`-...-` __...-." ANSI_GR "``-._|'`" ANSI_DR " _.-'|     " PORT_LINE ANSI_DR
" |    `-._   " ANSI_DW "`._    /" ANSI_DR "     _.-'    |     " PID_LINE ANSI_DR
"  `-._    `-._  " ANSI_DW "`-./" ANSI_DR "  _.-'    _.-'\n"
" |`-._`-._    `-"                    ".__.-'    _.-'_.-'|\n"
" |    `-._`-._  "                    "      _.-'_.-'    |           " URL_LINE ANSI_DR
"  `-._    `-._`-"                    ".__.-'_.-'    _.-'\n"
" |`-._`-._    `-"                    ".__.-'    _.-'_.-'|\n"
" |    `-._`-._  "                    "      _.-'_.-'    |\n"
"  `-._    `-._`-"                    ".__.-'_.-'    _.-'\n"
"      `-._    `-"                    ".__.-'    _.-'\n"
"          `-._  "                    "      _.-'\n"
"              `-"                    ".__.-'\n" ANSI_CL "\n";

char *ascii_logo =
"                _._                                                  \n"
"           _.-``__ ''-._                                             \n"
"      _.-``    `.  `_.  ''-._           Redis %s (%s/%d) %s bit\n"
"  .-`` .-```.  ```\\/    _.,_ ''-._                                   \n"
" (    '      ,       .-`  | `,    )     Running in %s mode\n"
" |`-._`-...-` __...-.``-._|'` _.-'|     Port: %d\n"
" |    `-._   `._    /     _.-'    |     PID: %ld\n"
"  `-._    `-._  `-./  _.-'    _.-'                                   \n"
" |`-._`-._    `-.__.-'    _.-'_.-'|                                  \n"
" |    `-._`-._        _.-'_.-'    |           http://redis.io        \n"
"  `-._    `-._`-.__.-'_.-'    _.-'                                   \n"
" |`-._`-._    `-.__.-'    _.-'_.-'|                                  \n"
" |    `-._`-._        _.-'_.-'    |                                  \n"
"  `-._    `-._`-.__.-'_.-'    _.-'                                   \n"
"      `-._    `-.__.-'    _.-'                                       \n"
"          `-._        _.-'                                           \n"
"              `-.__.-'                                               \n\n";
