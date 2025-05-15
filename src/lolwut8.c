/*
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Originally authored by: Salvatore Sanfilippo.
 * Algorithm based on the Almanacco Bompiani description and the Python
 * code written by Emiliano Russo.
 */

#include "server.h"
#include <ctype.h>

/* The LOLWUT 8 command:
 *
 * LOLWUT [EN|IT]
 *
 * By default the command produces verses in English language, in order for
 * the output to be more universally accessible. However, passing IT as argument
 * it is possible to reproduce the original output, exactly like done by
 * Nanni Balestrini in TAPE MARK I, and described in the Almanacco Letterario
 * Bompiani, 1962.
 */

// Structure to represent a verse with its metrical characteristics.
typedef struct {
    char text_en[100];    // English verse text.
    char text_it[100];    // Italian verse text.
    char fraction1[5];    // First fraction (rhythm/meter indicator).
    char fraction2[5];    // Second fraction (rhythm/meter indicator).
    char group[2];        // Group number (1-3 representing different
                          // literary sources).
} Verse;

// Fisher-Yates shuffle algorithm to randomize verse order.
static void shuffle(Verse *array, int size) {
    for (int i = size - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Verse temp = array[j];
        array[j] = array[i];
        array[i] = temp;
    }
}

void lolwut8Command(client *c) {
    int en_lang = 1;  // Default to English.

    /* Parse the optional arguments if any. */
    if (c->argc > 1 && !strcasecmp(c->argv[1]->ptr,"IT"))
        en_lang = 0;

    // Define verses from three literary sources with their metrical fractions:
    // Group 1: Diary of Hiroshima by Michihito Hachiya.
    // Group 2: The Mystery of the Elevator by Paul Goldwin.
    // Group 3: Tao Te Ching by Lao Tse.
    Verse verses[] = {
        // Group 1: Hiroshima verses.
        {" The blinding / globe / of fire ",
         " l accecante   /  globo  /  di fuoco  ", "1/4", "2/3", "1"},
        {" It expands / rapidly ",
         " si espande   /  rapidamente  ", "1/2", "3/4", "1"},
        {" Thirty times / brighter / than the sun ",
         " trenta volte  / piu luminoso  / del sole ", "2/3", "2/4", "1"},
        {" When it reaches / the stratosphere ",
         " quando  raggiunge / la stratosfera  ", "3/4", "1/2", "1"},
        {" The summit / of the cloud ",
         " la  sommita  /  della nuvola ", "1/3", "2/3", "1"},
        {" Assumes / the well-known shape / of a mushroom ",
         " assume   / la ben nota forma  / di fungo ", "2/4", "3/4", "1"},

        // Group 2: Elevator mystery verses.
        {" The head / pressed / upon the shoulder ",
         " la testa / premuta  / sulla spalla  ", "1/4", "2/4", "2"},
        {" The hair / between the lips ",
         " i  capelli   /  tra le labbra ", "1/4", "2/4", "2"},
        {" They lay / motionless / without speaking ",
         " giacquero  /   immobili / senza parlare ", "2/3", "2/3", "2"},
        {" Till he moved / his fingers / slowly ",
         " finche non mosse  /  le dita  / lentamente    ", "3/4", "1/3", "2"},
        {" Trying / to grasp ",
         " cercando / di afferrare  ", "3/4", "1/2", "2"},

        // Group 3: Tao Te Ching verses.
        {" While the multitude / of things / comes into being ",
         " mentre la moltitudine  /  delle cose  /   accade   ", "1/2", "1/2", "3"},
        {" I envisage / their return ",
         " io contemplo  /  il loro ritorno    ", "2/3", "3/4", "3"},
        {" Although / things / flourish ",
         " malgrado / che le cose  /  fioriscano    ", "1/2", "2/3", "3"},
        {" They all return / to / their roots ",
         " esse tornano  / tutte    / alla loro radice   ", "2/3", "1/4", "3"}
    };

    // Calculate the total number of verses.
    int num_verses = sizeof(verses) / sizeof(verses[0]);

    // Create a working copy of verses for manipulation.
    Verse *working_verses = zmalloc(num_verses * sizeof(Verse));
    memcpy(working_verses, verses, num_verses * sizeof(Verse));

    // Step 1: Shuffle the verses randomly.
    shuffle(working_verses, num_verses);

    // Step 2: Build stanza by finding compatible verses
    // Each subsequent verse must:
    // - Have compatible metrical fractions (connecting criteria).
    // - Belong to a different group than the previous verse.
    Verse stanza[10];

    int j; // At the end, it will contain the number of added stanzas.
    for (j = 0; j < 10; j++) {
        int i = 0;
        int found = 0;

        // Search for compatible verse among remaining verses.
        while (i < num_verses) {
            // Metrical compatibility check: this is used to select verses
            // that go somewhat well together, if their fractions match.
            // The algorithm checks if current verse's first fraction matches
            // with previous verse's second fraction in various ways, and
            // force successive verses to be of different groups.
            if (j == 0 || // First stanza is always accepted.
                ((working_verses[i].fraction1[0] == stanza[j-1].fraction2[0] ||
                  working_verses[i].fraction1[2] == stanza[j-1].fraction2[0] ||
                  working_verses[i].fraction1[2] == stanza[j-1].fraction2[2]) &&
                 strcmp(working_verses[i].group, stanza[j-1].group) != 0))
            {

                // Add compatible verse to stanza.
                stanza[j] = working_verses[i];

                // Remove selected verse from working set, to avoid reuse.
                for (int k = i; k < num_verses - 1; k++)
                    working_verses[k] = working_verses[k + 1];
                num_verses--;

                found = 1;
                break;
            }
            i++;
        }

        // Exit if there are no longer matching verses.
        if (!found) break;
    }
    zfree(working_verses);

    // Step 3: Combine all stanza verses into single SDS string.
    sds combined = sdsempty();
    for (int i = 0; i < j; i++) {
        if (en_lang) {
            combined = sdscat(combined, stanza[i].text_en);
        } else {
            combined = sdscat(combined, stanza[i].text_it);
        }
        combined = sdscat(combined, "\n");
    }

    // Step 4: Make uppercase, and strip the "/".
    for (size_t j = 0; j < sdslen(combined); j++) {
        combined[j] = toupper(combined[j]);
        if (combined[j] == '/') combined[j] = ' ';
    }

    // Step 5: Add background info about what the user just saw.
    combined = sdscat(combined,
        "\nIn 1961, Nanni Balestrini created one of the first computer-generated poems, TAPE MARK I, using an IBM 7090 mainframe. Each execution combined verses from three literary sources following algorithmic rules based on metrical compatibility and group constraints. This LOLWUT command reproduces Balestrini's original algorithm, generating new stanzas through the same computational poetry process described in Almanacco Letterario Bompiani, 1962.\n\n"
        "https://en.wikipedia.org/wiki/Digital_poetry\n"
        "https://www.youtube.com/watch?v=8i7uFCK7G0o (English subs)\n\n"
        "Use: LOLWUT IT for the original Italian output.\n\n");

    addReplyVerbatim(c,combined,sdslen(combined),"txt");
    sdsfree(combined);
}
