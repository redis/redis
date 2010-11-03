#include <stdio.h>
#include <stdlib.h>
#include "linenoise.h"

int main(void) {
    char *line;

    linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
    while((line = linenoise("hello> ")) != NULL) {
        if (line[0] != '\0') {
            printf("echo: '%s'\n", line);
            linenoiseHistoryAdd(line);
            linenoiseHistorySave("history.txt"); /* Save every new entry */
        }
        free(line);
    }
    return 0;
}
