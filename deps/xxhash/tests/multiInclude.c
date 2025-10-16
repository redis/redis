/*
 * Multi-include test program
 * Validates that xxhash.h can be included multiple times and in any order
 *
 * Copyright (C) 2020 Yann Collet
 *
 * GPL v2 License
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * You can contact the author at:
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

#include <stdio.h>   /* printf */

/* Normal include, gives access to public symbols */
#include "../xxhash.h"

/* Multiple consecutive inclusions are handled properly. */
#include "../xxhash.h"

/*
 * Advanced include, gives access to experimental symbols
 * This test ensures that xxhash.h can be included multiple times
 * and in any order. The tested order is more difficult:
 * without care, the declaration of experimental symbols could be skipped.
 */
#define XXH_STATIC_LINKING_ONLY
#include "../xxhash.h"

/*
 * Inlining: redefine all identifiers, keep them private to the unit.
 * Note: Without specific efforts, the identifier names would collide.
 *
 * To be linked with and without xxhash.o
 * to test the symbol's presence and naming collisions.
 */
#define XXH_INLINE_ALL
#include "../xxhash.h"

/*
 * Multiple consecutive inclusions with XXH_INLINE_ALL are handled properly.
 */
#define XXH_INLINE_ALL
#include "../xxhash.h"


void hash_advanced(void)
{
    const char input[] = "Hello World !";
    XXH3_state_t state;   /* this type is part of experimental API */

    XXH3_64bits_reset(&state);

    XXH3_64bits_update(&state, input, sizeof(input));

    {   XXH64_hash_t const h = XXH3_64bits_digest(&state);
        printf("hash '%s': %08x%08x \n", input, (unsigned)(h >> 32), (unsigned)h);
    }
}

int main(void)
{
    hash_advanced();
}
