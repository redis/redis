/*
* Copyright (c), Microsoft Open Technologies, Inc.
* All rights reserved.
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*  - Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*  - Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Derived from ANSI.c by Jason Hood, from his ansicon project (https://github.com/adoxa/ansicon), with modifications. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "Win32_ANSI.h"

#define lenof(array) (sizeof(array)/sizeof(*(array)))

typedef struct {
    BYTE foreground;	// ANSI base color (0 to 7; add 30)
    BYTE background;	// ANSI base color (0 to 7; add 40)
    BYTE bold;	// console FOREGROUND_INTENSITY bit
    BYTE underline;	// console BACKGROUND_INTENSITY bit
    BYTE rvideo;	// swap foreground/bold & background/underline
    BYTE concealed;	// set foreground/bold to background/underline
    BYTE reverse; // swap console foreground & background attributes
} GRM, *PGRM;	// Graphic Rendition Mode


#define is_digit(c) ('0' <= (c) && (c) <= '9')

// ========== Global variables and constants

HANDLE	  hConOut;		// handle to CONOUT$

#define ESC	'\x1B'          // ESCape character
#define BEL	'\x07'
#define SO	'\x0E'          // Shift Out
#define SI	'\x0F'          // Shift In

#define MAX_ARG 16		// max number of args in an escape sequence
int   state;			// automata state
TCHAR prefix;			// escape sequence prefix ( '[', ']' or '(' );
TCHAR prefix2;			// secondary prefix ( '?' or '>' );
TCHAR suffix;			// escape sequence suffix
int   es_argc;			// escape sequence args count
int   es_argv[MAX_ARG]; 	// escape sequence args
TCHAR Pt_arg[MAX_PATH * 2];	// text parameter for Operating System Command
int   Pt_len;
BOOL  shifted;


// DEC Special Graphics Character Set from
// http://vt100.net/docs/vt220-rm/table2-4.html
// Some of these may not look right, depending on the font and code page (in
// particular, the Control Pictures probably won't work at all).
const WCHAR G1[] =
{
    ' ',          // _ - blank
    L'\x2666',    // ` - Black Diamond Suit
    L'\x2592',    // a - Medium Shade
    L'\x2409',    // b - HT
    L'\x240c',    // c - FF
    L'\x240d',    // d - CR
    L'\x240a',    // e - LF
    L'\x00b0',    // f - Degree Sign
    L'\x00b1',    // g - Plus-Minus Sign
    L'\x2424',    // h - NL
    L'\x240b',    // i - VT
    L'\x2518',    // j - Box Drawings Light Up And Left
    L'\x2510',    // k - Box Drawings Light Down And Left
    L'\x250c',    // l - Box Drawings Light Down And Right
    L'\x2514',    // m - Box Drawings Light Up And Right
    L'\x253c',    // n - Box Drawings Light Vertical And Horizontal
    L'\x00af',    // o - SCAN 1 - Macron
    L'\x25ac',    // p - SCAN 3 - Black Rectangle
    L'\x2500',    // q - SCAN 5 - Box Drawings Light Horizontal
    L'_',         // r - SCAN 7 - Low Line
    L'_',         // s - SCAN 9 - Low Line
    L'\x251c',    // t - Box Drawings Light Vertical And Right
    L'\x2524',    // u - Box Drawings Light Vertical And Left
    L'\x2534',    // v - Box Drawings Light Up And Horizontal
    L'\x252c',    // w - Box Drawings Light Down And Horizontal
    L'\x2502',    // x - Box Drawings Light Vertical
    L'\x2264',    // y - Less-Than Or Equal To
    L'\x2265',    // z - Greater-Than Or Equal To
    L'\x03c0',    // { - Greek Small Letter Pi
    L'\x2260',    // | - Not Equal To
    L'\x00a3',    // } - Pound Sign
    L'\x00b7',    // ~ - Middle Dot
};

#define FIRST_G1 '_'
#define LAST_G1  '~'


// color constants

#define FOREGROUND_BLACK 0
#define FOREGROUND_WHITE FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE

#define BACKGROUND_BLACK 0
#define BACKGROUND_WHITE BACKGROUND_RED|BACKGROUND_GREEN|BACKGROUND_BLUE

const BYTE foregroundcolor[8] =
{
    FOREGROUND_BLACK,			// black foreground
    FOREGROUND_RED,			// red foreground
    FOREGROUND_GREEN,			// green foreground
    FOREGROUND_RED | FOREGROUND_GREEN,	// yellow foreground
    FOREGROUND_BLUE,			// blue foreground
    FOREGROUND_BLUE | FOREGROUND_RED,	// magenta foreground
    FOREGROUND_BLUE | FOREGROUND_GREEN,	// cyan foreground
    FOREGROUND_WHITE			// white foreground
};

const BYTE backgroundcolor[8] =
{
    BACKGROUND_BLACK,			// black background
    BACKGROUND_RED,			// red background
    BACKGROUND_GREEN,			// green background
    BACKGROUND_RED | BACKGROUND_GREEN,	// yellow background
    BACKGROUND_BLUE,			// blue background
    BACKGROUND_BLUE | BACKGROUND_RED,	// magenta background
    BACKGROUND_BLUE | BACKGROUND_GREEN,	// cyan background
    BACKGROUND_WHITE,			// white background
};

const BYTE attr2ansi[8] =		// map console attribute to ANSI number
{
    0,					// black
    4,					// blue
    2,					// green
    6,					// cyan
    1,					// red
    5,					// magenta
    3,					// yellow
    7					// white
};

GRM grm;

// saved cursor position
COORD SavePos;

// ========== Print Buffer functions

#define BUFFER_SIZE 2048

int   nCharInBuffer;
WCHAR ChBuffer[BUFFER_SIZE];

//-----------------------------------------------------------------------------
//   FlushBuffer()
// Writes the buffer to the console and empties it.
//-----------------------------------------------------------------------------

void FlushBuffer(void) {
    DWORD nWritten;
    if (nCharInBuffer <= 0) return;
    WriteConsole(hConOut, ChBuffer, nCharInBuffer, &nWritten, NULL);
    nCharInBuffer = 0;
}

//-----------------------------------------------------------------------------
//   PushBuffer( WCHAR c )
// Adds a character in the buffer.
//-----------------------------------------------------------------------------

void PushBuffer(WCHAR c) {
    if (shifted && c >= FIRST_G1 && c <= LAST_G1)
        c = G1[c - FIRST_G1];
    ChBuffer[nCharInBuffer] = c;
    if (++nCharInBuffer == BUFFER_SIZE)
        FlushBuffer();
}

//-----------------------------------------------------------------------------
//   SendSequence( LPTSTR seq )
// Send the string to the input buffer.
//-----------------------------------------------------------------------------

void SendSequence(LPTSTR seq) {
    DWORD out;
    INPUT_RECORD in;
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);

    in.EventType = KEY_EVENT;
    in.Event.KeyEvent.bKeyDown = TRUE;
    in.Event.KeyEvent.wRepeatCount = 1;
    in.Event.KeyEvent.wVirtualKeyCode = 0;
    in.Event.KeyEvent.wVirtualScanCode = 0;
    in.Event.KeyEvent.dwControlKeyState = 0;
    for (; *seq; ++seq) {
        in.Event.KeyEvent.uChar.UnicodeChar = *seq;
        WriteConsoleInput(hStdIn, &in, 1, &out);
    }
}

// ========== Print functions

//-----------------------------------------------------------------------------
//   InterpretEscSeq()
// Interprets the last escape sequence scanned by ParseAndPrintANSIString
//   prefix             escape sequence prefix
//   es_argc            escape sequence args count
//   es_argv[]          escape sequence args array
//   suffix             escape sequence suffix
//
// for instance, with \e[33;45;1m we have
// prefix = '[',
// es_argc = 3, es_argv[0] = 33, es_argv[1] = 45, es_argv[2] = 1
// suffix = 'm'
//-----------------------------------------------------------------------------

void InterpretEscSeq(void) {
    int  i;
    WORD attribut;
    CONSOLE_SCREEN_BUFFER_INFO Info;
    CONSOLE_CURSOR_INFO CursInfo;
    DWORD len, NumberOfCharsWritten;
    COORD Pos;
    SMALL_RECT Rect;
    CHAR_INFO  CharInfo;

    if (prefix == '[') {
        if (prefix2 == '?' && (suffix == 'h' || suffix == 'l')) {
            if (es_argc == 1 && es_argv[0] == 25) {
                GetConsoleCursorInfo(hConOut, &CursInfo);
                CursInfo.bVisible = (suffix == 'h');
                SetConsoleCursorInfo(hConOut, &CursInfo);
                return;
            }
        }
        // Ignore any other \e[? or \e[> sequences.
        if (prefix2 != 0)
            return;

        GetConsoleScreenBufferInfo(hConOut, &Info);
        switch (suffix) {
            case 'm':
                if (es_argc == 0) es_argv[es_argc++] = 0;
                for (i = 0; i < es_argc; i++) {
                    if (30 <= es_argv[i] && es_argv[i] <= 37)
                        grm.foreground = es_argv[i] - 30;
                    else if (40 <= es_argv[i] && es_argv[i] <= 47)
                        grm.background = es_argv[i] - 40;
                    else switch (es_argv[i]) {
                        case 0:
                        case 39:
                        case 49:
                        {
                                   TCHAR def[4];
                                   int   a;
                                   *def = '7'; def[1] = '\0';
                                   GetEnvironmentVariable(L"ANSICON_DEF", def, lenof(def));
                                   a = wcstol(def, NULL, 16);
                                   grm.reverse = FALSE;
                                   if (a < 0) {
                                       grm.reverse = TRUE;
                                       a = -a;
                                   }
                                   if (es_argv[i] != 49)
                                       grm.foreground = attr2ansi[a & 7];
                                   if (es_argv[i] != 39)
                                       grm.background = attr2ansi[(a >> 4) & 7];
                                   if (es_argv[i] == 0) {
                                       if (es_argc == 1) {
                                           grm.bold = a & FOREGROUND_INTENSITY;
                                           grm.underline = a & BACKGROUND_INTENSITY;
                                       } else {
                                           grm.bold = 0;
                                           grm.underline = 0;
                                       }
                                       grm.rvideo = 0;
                                       grm.concealed = 0;
                                   }
                        }
                            break;

                        case  1: grm.bold = FOREGROUND_INTENSITY; break;
                        case  5: // blink
                        case  4: grm.underline = BACKGROUND_INTENSITY; break;
                        case  7: grm.rvideo = 1; break;
                        case  8: grm.concealed = 1; break;
                        case 21: // oops, this actually turns on double underline
                        case 22: grm.bold = 0; break;
                        case 25:
                        case 24: grm.underline = 0; break;
                        case 27: grm.rvideo = 0; break;
                        case 28: grm.concealed = 0; break;
                    }
                }
                if (grm.concealed) {
                    if (grm.rvideo) {
                        attribut = foregroundcolor[grm.foreground]
                            | backgroundcolor[grm.foreground];
                        if (grm.bold)
                            attribut |= FOREGROUND_INTENSITY | BACKGROUND_INTENSITY;
                    } else {
                        attribut = foregroundcolor[grm.background]
                            | backgroundcolor[grm.background];
                        if (grm.underline)
                            attribut |= FOREGROUND_INTENSITY | BACKGROUND_INTENSITY;
                    }
                } else if (grm.rvideo) {
                    attribut = foregroundcolor[grm.background]
                        | backgroundcolor[grm.foreground];
                    if (grm.bold)
                        attribut |= BACKGROUND_INTENSITY;
                    if (grm.underline)
                        attribut |= FOREGROUND_INTENSITY;
                } else
                    attribut = foregroundcolor[grm.foreground] | grm.bold
                    | backgroundcolor[grm.background] | grm.underline;
                if (grm.reverse)
                    attribut = ((attribut >> 4) & 15) | ((attribut & 15) << 4);
                SetConsoleTextAttribute(hConOut, attribut);
                return;

            case 'J':
                if (es_argc == 0) es_argv[es_argc++] = 0; // ESC[J == ESC[0J
                if (es_argc != 1) return;
                switch (es_argv[0]) {
                    case 0:		// ESC[0J erase from cursor to end of display
                        len = (Info.dwSize.Y - Info.dwCursorPosition.Y - 1) * Info.dwSize.X
                            + Info.dwSize.X - Info.dwCursorPosition.X - 1;
                        FillConsoleOutputCharacter(hConOut, ' ', len,
                            Info.dwCursorPosition,
                            &NumberOfCharsWritten);
                        FillConsoleOutputAttribute(hConOut, Info.wAttributes, len,
                            Info.dwCursorPosition,
                            &NumberOfCharsWritten);
                        return;

                    case 1:		// ESC[1J erase from start to cursor.
                        Pos.X = 0;
                        Pos.Y = 0;
                        len = Info.dwCursorPosition.Y * Info.dwSize.X
                            + Info.dwCursorPosition.X + 1;
                        FillConsoleOutputCharacter(hConOut, ' ', len, Pos,
                            &NumberOfCharsWritten);
                        FillConsoleOutputAttribute(hConOut, Info.wAttributes, len, Pos,
                            &NumberOfCharsWritten);
                        return;

                    case 2:		// ESC[2J Clear screen and home cursor
                        Pos.X = 0;
                        Pos.Y = 0;
                        len = Info.dwSize.X * Info.dwSize.Y;
                        FillConsoleOutputCharacter(hConOut, ' ', len, Pos,
                            &NumberOfCharsWritten);
                        FillConsoleOutputAttribute(hConOut, Info.wAttributes, len, Pos,
                            &NumberOfCharsWritten);
                        SetConsoleCursorPosition(hConOut, Pos);
                        return;

                    default:
                        return;
                }

            case 'K':
                if (es_argc == 0) es_argv[es_argc++] = 0; // ESC[K == ESC[0K
                if (es_argc != 1) return;
                switch (es_argv[0]) {
                    case 0:		// ESC[0K Clear to end of line
                        len = Info.dwSize.X - Info.dwCursorPosition.X + 1;
                        FillConsoleOutputCharacter(hConOut, ' ', len,
                            Info.dwCursorPosition,
                            &NumberOfCharsWritten);
                        FillConsoleOutputAttribute(hConOut, Info.wAttributes, len,
                            Info.dwCursorPosition,
                            &NumberOfCharsWritten);
                        return;

                    case 1:		// ESC[1K Clear from start of line to cursor
                        Pos.X = 0;
                        Pos.Y = Info.dwCursorPosition.Y;
                        FillConsoleOutputCharacter(hConOut, ' ',
                            Info.dwCursorPosition.X + 1, Pos,
                            &NumberOfCharsWritten);
                        FillConsoleOutputAttribute(hConOut, Info.wAttributes,
                            Info.dwCursorPosition.X + 1, Pos,
                            &NumberOfCharsWritten);
                        return;

                    case 2:		// ESC[2K Clear whole line.
                        Pos.X = 0;
                        Pos.Y = Info.dwCursorPosition.Y;
                        FillConsoleOutputCharacter(hConOut, ' ', Info.dwSize.X, Pos,
                            &NumberOfCharsWritten);
                        FillConsoleOutputAttribute(hConOut, Info.wAttributes,
                            Info.dwSize.X, Pos,
                            &NumberOfCharsWritten);
                        return;

                    default:
                        return;
                }

            case 'X':                 // ESC[#X Erase # characters.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[X == ESC[1X
                if (es_argc != 1) return;
                FillConsoleOutputCharacter(hConOut, ' ', es_argv[0],
                    Info.dwCursorPosition,
                    &NumberOfCharsWritten);
                FillConsoleOutputAttribute(hConOut, Info.wAttributes, es_argv[0],
                    Info.dwCursorPosition,
                    &NumberOfCharsWritten);
                return;

            case 'L':                 // ESC[#L Insert # blank lines.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[L == ESC[1L
                if (es_argc != 1) return;
                Rect.Left = 0;
                Rect.Top = Info.dwCursorPosition.Y;
                Rect.Right = Info.dwSize.X - 1;
                Rect.Bottom = Info.dwSize.Y - 1;
                Pos.X = 0;
                Pos.Y = Info.dwCursorPosition.Y + es_argv[0];
                CharInfo.Char.UnicodeChar = ' ';
                CharInfo.Attributes = Info.wAttributes;
                ScrollConsoleScreenBuffer(hConOut, &Rect, NULL, Pos, &CharInfo);
                return;

            case 'M':                 // ESC[#M Delete # lines.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[M == ESC[1M
                if (es_argc != 1) return;
                if (es_argv[0] > Info.dwSize.Y - Info.dwCursorPosition.Y)
                    es_argv[0] = Info.dwSize.Y - Info.dwCursorPosition.Y;
                Rect.Left = 0;
                Rect.Top = Info.dwCursorPosition.Y + es_argv[0];
                Rect.Right = Info.dwSize.X - 1;
                Rect.Bottom = Info.dwSize.Y - 1;
                Pos.X = 0;
                Pos.Y = Info.dwCursorPosition.Y;
                CharInfo.Char.UnicodeChar = ' ';
                CharInfo.Attributes = Info.wAttributes;
                ScrollConsoleScreenBuffer(hConOut, &Rect, NULL, Pos, &CharInfo);
                return;

            case 'P':                 // ESC[#P Delete # characters.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[P == ESC[1P
                if (es_argc != 1) return;
                if (Info.dwCursorPosition.X + es_argv[0] > Info.dwSize.X - 1)
                    es_argv[0] = Info.dwSize.X - Info.dwCursorPosition.X;
                Rect.Left = Info.dwCursorPosition.X + es_argv[0];
                Rect.Top = Info.dwCursorPosition.Y;
                Rect.Right = Info.dwSize.X - 1;
                Rect.Bottom = Info.dwCursorPosition.Y;
                CharInfo.Char.UnicodeChar = ' ';
                CharInfo.Attributes = Info.wAttributes;
                ScrollConsoleScreenBuffer(hConOut, &Rect, NULL, Info.dwCursorPosition,
                    &CharInfo);
                return;

            case '@':                 // ESC[#@ Insert # blank characters.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[@ == ESC[1@
                if (es_argc != 1) return;
                if (Info.dwCursorPosition.X + es_argv[0] > Info.dwSize.X - 1)
                    es_argv[0] = Info.dwSize.X - Info.dwCursorPosition.X;
                Rect.Left = Info.dwCursorPosition.X;
                Rect.Top = Info.dwCursorPosition.Y;
                Rect.Right = Info.dwSize.X - 1 - es_argv[0];
                Rect.Bottom = Info.dwCursorPosition.Y;
                Pos.X = Info.dwCursorPosition.X + es_argv[0];
                Pos.Y = Info.dwCursorPosition.Y;
                CharInfo.Char.UnicodeChar = ' ';
                CharInfo.Attributes = Info.wAttributes;
                ScrollConsoleScreenBuffer(hConOut, &Rect, NULL, Pos, &CharInfo);
                return;

            case 'k':                 // ESC[#k
            case 'A':                 // ESC[#A Moves cursor up # lines
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[A == ESC[1A
                if (es_argc != 1) return;
                Pos.Y = Info.dwCursorPosition.Y - es_argv[0];
                if (Pos.Y < 0) Pos.Y = 0;
                Pos.X = Info.dwCursorPosition.X;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 'e':                 // ESC[#e
            case 'B':                 // ESC[#B Moves cursor down # lines
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[B == ESC[1B
                if (es_argc != 1) return;
                Pos.Y = Info.dwCursorPosition.Y + es_argv[0];
                if (Pos.Y >= Info.dwSize.Y) Pos.Y = Info.dwSize.Y - 1;
                Pos.X = Info.dwCursorPosition.X;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 'a':                 // ESC[#a
            case 'C':                 // ESC[#C Moves cursor forward # spaces
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[C == ESC[1C
                if (es_argc != 1) return;
                Pos.X = Info.dwCursorPosition.X + es_argv[0];
                if (Pos.X >= Info.dwSize.X) Pos.X = Info.dwSize.X - 1;
                Pos.Y = Info.dwCursorPosition.Y;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 'j':                 // ESC[#j
            case 'D':                 // ESC[#D Moves cursor back # spaces
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[D == ESC[1D
                if (es_argc != 1) return;
                Pos.X = Info.dwCursorPosition.X - es_argv[0];
                if (Pos.X < 0) Pos.X = 0;
                Pos.Y = Info.dwCursorPosition.Y;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 'E':                 // ESC[#E Moves cursor down # lines, column 1.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[E == ESC[1E
                if (es_argc != 1) return;
                Pos.Y = Info.dwCursorPosition.Y + es_argv[0];
                if (Pos.Y >= Info.dwSize.Y) Pos.Y = Info.dwSize.Y - 1;
                Pos.X = 0;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 'F':                 // ESC[#F Moves cursor up # lines, column 1.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[F == ESC[1F
                if (es_argc != 1) return;
                Pos.Y = Info.dwCursorPosition.Y - es_argv[0];
                if (Pos.Y < 0) Pos.Y = 0;
                Pos.X = 0;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case '`':                 // ESC[#`
            case 'G':                 // ESC[#G Moves cursor column # in current row.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[G == ESC[1G
                if (es_argc != 1) return;
                Pos.X = es_argv[0] - 1;
                if (Pos.X >= Info.dwSize.X) Pos.X = Info.dwSize.X - 1;
                if (Pos.X < 0) Pos.X = 0;
                Pos.Y = Info.dwCursorPosition.Y;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 'd':                 // ESC[#d Moves cursor row #, current column.
                if (es_argc == 0) es_argv[es_argc++] = 1; // ESC[d == ESC[1d
                if (es_argc != 1) return;
                Pos.Y = es_argv[0] - 1;
                if (Pos.Y < 0) Pos.Y = 0;
                if (Pos.Y >= Info.dwSize.Y) Pos.Y = Info.dwSize.Y - 1;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 'f':                 // ESC[#;#f
            case 'H':                 // ESC[#;#H Moves cursor to line #, column #
                if (es_argc == 0)
                    es_argv[es_argc++] = 1; // ESC[H == ESC[1;1H
                if (es_argc == 1)
                    es_argv[es_argc++] = 1; // ESC[#H == ESC[#;1H
                if (es_argc > 2) return;
                Pos.X = es_argv[1] - 1;
                if (Pos.X < 0) Pos.X = 0;
                if (Pos.X >= Info.dwSize.X) Pos.X = Info.dwSize.X - 1;
                Pos.Y = es_argv[0] - 1;
                if (Pos.Y < 0) Pos.Y = 0;
                if (Pos.Y >= Info.dwSize.Y) Pos.Y = Info.dwSize.Y - 1;
                SetConsoleCursorPosition(hConOut, Pos);
                return;

            case 's':                 // ESC[s Saves cursor position for recall later
                if (es_argc != 0) return;
                SavePos = Info.dwCursorPosition;
                return;

            case 'u':                 // ESC[u Return to saved cursor position
                if (es_argc != 0) return;
                SetConsoleCursorPosition(hConOut, SavePos);
                return;

            case 'n':                 // ESC[#n Device status report
                if (es_argc != 1) return; // ESC[n == ESC[0n -> ignored
                switch (es_argv[0]) {
                    case 5:		// ESC[5n Report status
                        SendSequence(L"\33[0n"); // "OK"
                        return;

                    case 6:		// ESC[6n Report cursor position
                    {
                                    TCHAR buf[32];
                                    wsprintf(buf, L"\33[%d;%dR", Info.dwCursorPosition.Y + 1,
                                        Info.dwCursorPosition.X + 1);
                                    SendSequence(buf);
                    }
                        return;

                    default:
                        return;
                }

            case 't':                 // ESC[#t Window manipulation
                if (es_argc != 1) return;
                if (es_argv[0] == 21)	// ESC[21t Report xterm window's title
                {
                    TCHAR buf[MAX_PATH * 2];
                    DWORD len = GetConsoleTitle(buf + 3, lenof(buf) - 3 - 2);
                    // Too bad if it's too big or fails.
                    buf[0] = ESC;
                    buf[1] = ']';
                    buf[2] = 'l';
                    buf[3 + len] = ESC;
                    buf[3 + len + 1] = '\\';
                    buf[3 + len + 2] = '\0';
                    SendSequence(buf);
                }
                return;

            default:
                return;
        }
    } else // (prefix == ']')
    {
        // Ignore any \e]? or \e]> sequences.
        if (prefix2 != 0)
            return;

        if (es_argc == 1 && es_argv[0] == 0) // ESC]0;titleST
        {
            SetConsoleTitle(Pt_arg);
        }
    }
}

//-----------------------------------------------------------------------------
//   ParseAndPrintANSIString(hDev, lpBuffer, nNumberOfBytesToWrite)
// Parses the string lpBuffer, interprets the escapes sequences and prints the
// characters in the device hDev (console).
// The lexer is a three states automata.
// If the number of arguments es_argc > MAX_ARG, only the MAX_ARG-1 firsts and
// the last arguments are processed (no es_argv[] overflow).
//-----------------------------------------------------------------------------
BOOL ParseAndPrintANSIString(HANDLE hDev, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten) {
    DWORD   i;
    LPCSTR s;

    if (hDev != hConOut)	// reinit if device has changed
    {
        hConOut = hDev;
        state = 1;
        shifted = FALSE;
    }
    for (i = nNumberOfBytesToWrite, s = (LPCSTR)lpBuffer; i > 0; i--, s++) {
        if (state == 1) {
            if (*s == ESC) state = 2;
            else if (*s == SO) shifted = TRUE;
            else if (*s == SI) shifted = FALSE;
            else PushBuffer(*s);
        } else if (state == 2) {
            if (*s == ESC);	// \e\e...\e == \e
            else if ((*s == '[') || (*s == ']')) {
                FlushBuffer();
                prefix = *s;
                prefix2 = 0;
                state = 3;
                Pt_len = 0;
                *Pt_arg = '\0';
            } else if (*s == ')' || *s == '(') state = 6;
            else state = 1;
        } else if (state == 3) {
            if (is_digit(*s)) {
                es_argc = 0;
                es_argv[0] = *s - '0';
                state = 4;
            } else if (*s == ';') {
                es_argc = 1;
                es_argv[0] = 0;
                es_argv[1] = 0;
                state = 4;
            } else if (*s == '?' || *s == '>') {
                prefix2 = *s;
            } else {
                es_argc = 0;
                suffix = *s;
                InterpretEscSeq();
                state = 1;
            }
        } else if (state == 4) {
            if (is_digit(*s)) {
                es_argv[es_argc] = 10 * es_argv[es_argc] + (*s - '0');
            } else if (*s == ';') {
                if (es_argc < MAX_ARG - 1) es_argc++;
                es_argv[es_argc] = 0;
                if (prefix == ']')
                    state = 5;
            } else {
                es_argc++;
                suffix = *s;
                InterpretEscSeq();
                state = 1;
            }
        } else if (state == 5) {
            if (*s == BEL) {
                Pt_arg[Pt_len] = '\0';
                InterpretEscSeq();
                state = 1;
            } else if (*s == '\\' && Pt_len > 0 && Pt_arg[Pt_len - 1] == ESC) {
                Pt_arg[--Pt_len] = '\0';
                InterpretEscSeq();
                state = 1;
            } else if (Pt_len < lenof(Pt_arg) - 1)
                Pt_arg[Pt_len++] = *s;
        } else if (state == 6) {
            // Ignore it (ESC ) 0 is implicit; nothing else is supported).
            state = 1;
        }
    }
    FlushBuffer();
    if (lpNumberOfBytesWritten != NULL)
        *lpNumberOfBytesWritten = nNumberOfBytesToWrite - i;
    return (i == 0);
}

void ANSI_printf(char *format, ...) {
    va_list args;
    int retVal;
#define cBufLen 2000
    char buffer[cBufLen];
    memset(buffer, 0, cBufLen);

    va_start(args, format);
    retVal = vsprintf_s(buffer, cBufLen, format, args);
    va_end(args);

    if (retVal > 0) {
        DWORD bytesWritten = 0;
        ParseAndPrintANSIString(GetStdHandle(STD_OUTPUT_HANDLE), buffer, (DWORD)strlen(buffer), &bytesWritten);
    }
}


