#pragma once
//
//  Values are 32 bit values laid out as follows:
//
//   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
//   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
//  +---+-+-+-----------------------+-------------------------------+
//  |Sev|C|R|     Facility          |               Code            |
//  +---+-+-+-----------------------+-------------------------------+
//
//  where
//
//      Sev - is the severity code
//
//          00 - Success
//          01 - Informational
//          10 - Warning
//          11 - Error
//
//      C - is the Customer code flag
//
//      R - is a reserved bit
//
//      Facility - is the facility code
//
//      Code - is the facility's status code
//
//
// Define the facility codes
//


//
// Define the severity codes
//


//
// MessageId: MSG_INFO_1
//
// MessageText:
//
// %1
//
#define MSG_INFO_1                       0x60000000L

//
// MessageId: MSG_WARNING_1
//
// MessageText:
//
// %1
//
#define MSG_WARNING_1                    0xA0000001L

//
// MessageId: MSG_ERROR_1
//
// MessageText:
//
// %1
//
#define MSG_ERROR_1                      0xE0000002L

//
// MessageId: MSG_SUCCESS_1
//
// MessageText:
//
// %1
//
#define MSG_SUCCESS_1                    0x20000003L

