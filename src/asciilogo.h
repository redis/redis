/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

const char *ascii_logo =
"          ⠠⠴⠶⠾⠿⠿⠿⢶⣦⣄⡀                                            \n"
"                    ⠉⢿⣿⣆⠐⣄                                         \n"
"                       ⠿⠿⠆⠹⠦                                         \n"
"              ⢀⣤⣤⣤⣤⣀⠐⣶⣶⣶⣶⣶⣶⡀⢀⣀⣀                                     \n"
"                ⠉⠛⠻⢿⣿⡆⢹⡿⠻⢿⣿⣿⣷⠈⠿⠛⠁      Redis G.O.A.T Edition       \n"
"          ⢀⣀⣤⣴⣾⣷⣤⣉⣠⣾⣷⣦⣼⣿⣿⣿⣧             %s (%s/%d) %s bit         \n"
"  ⣶⣶⣶⣶⣶⣶⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣇        Running in %s mode       \n"
"  ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡄      Port: %d                 \n"
"  ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣛⠻⢧⣘⡷      PID: %ld                 \n"
"  ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠋⠀⠀⣉⠛⠿⣷⣦⣌⠁      Muhahaha                          \n"
"  ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠋⣠⠘⠀⠀⢹⣿⣶⣶          https://redis.io         \n"
"  ⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠋⠀⢺⣿⠀⠀⠀⠘⣿⣿⡟                                 \n"
"  ⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠋⠀⠀⠀⠀⠁⠀⠀⠀⠀⠻⡟⠃                                 \n"
"  ⠛⠛⠛⠛⠛⠛⠛⠛⠋                                                                \n";
