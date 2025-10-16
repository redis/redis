module xxhash where

/**
 * The 32-bit variant of xxHash. The first argument is the sequence
 * of L bytes to hash. The second argument is a seed value.
 */
XXH32 : {L} (fin L) => [L][8] -> [32] -> [32]
XXH32 input seed = XXH32_avalanche acc1
  where (stripes16 # stripes4 # stripes1) = input
        accR = foldl XXH32_rounds (XXH32_init seed) (split stripes16 : [L/16][16][8])
        accL = `(L % 2^^32) + if (`L:Integer) < 16
                              then seed + PRIME32_5
                              else XXH32_converge accR
        acc4 = foldl XXH32_digest4 accL (split stripes4 : [(L%16)/4][4][8])
        acc1 = foldl XXH32_digest1 acc4 (stripes1 : [L%4][8])

/**
 * The 64-bit variant of xxHash. The first argument is the sequence
 * of L bytes to hash. The second argument is a seed value.
 */
XXH64 : {L} (fin L) => [L][8] -> [64] -> [64]
XXH64 input seed = XXH64_avalanche acc1
  where (stripes32 # stripes8 # stripes4 # stripes1) = input
        accR = foldl XXH64_rounds (XXH64_init seed) (split stripes32 : [L/32][32][8])
        accL = `(L % 2^^64) + if (`L:Integer) < 32
                              then seed + PRIME64_5
                              else XXH64_converge accR
        acc8 = foldl XXH64_digest8 accL (split stripes8 : [(L%32)/8][8][8])
        acc4 = foldl XXH64_digest4 acc8 (split stripes4 : [(L%8)/4][4][8])
        acc1 = foldl XXH64_digest1 acc4 (stripes1 : [L%4][8])

private

  //Utility functions

  /**
   * Combines a sequence of bytes into a word using the little-endian
   * convention.
   */
  toLE bytes = join (reverse bytes)

  //32-bit xxHash helper functions

  //32-bit prime number constants
  PRIME32_1 = 0x9E3779B1 : [32]
  PRIME32_2 = 0x85EBCA77 : [32]
  PRIME32_3 = 0xC2B2AE3D : [32]
  PRIME32_4 = 0x27D4EB2F : [32]
  PRIME32_5 = 0x165667B1 : [32]

  /**
   * The property shows that the hexadecimal representation of the
   * PRIME32 constants is the same as the binary representation.
   */
  property PRIME32s_as_bits_correct =
    (PRIME32_1 == 0b10011110001101110111100110110001) /\
    (PRIME32_2 == 0b10000101111010111100101001110111) /\
    (PRIME32_3 == 0b11000010101100101010111000111101) /\
    (PRIME32_4 == 0b00100111110101001110101100101111) /\
    (PRIME32_5 == 0b00010110010101100110011110110001)

  /**
   * This function initializes the four internal accumulators of XXH32.
   */
  XXH32_init : [32] -> [4][32]
  XXH32_init seed = [acc1, acc2, acc3, acc4]
    where acc1 = seed + PRIME32_1 + PRIME32_2
          acc2 = seed + PRIME32_2
          acc3 = seed + 0
          acc4 = seed - PRIME32_1

  /**
   * This processes a single lane of the main round function of XXH32.
   */
  XXH32_round : [32] -> [32] -> [32]
  XXH32_round accN laneN = ((accN + laneN * PRIME32_2) <<< 13) * PRIME32_1

  /**
   * This is the main round function of XXH32 and processes a stripe,
   * i.e. 4 lanes with 4 bytes each.
   */
  XXH32_rounds : [4][32] -> [16][8] -> [4][32]
  XXH32_rounds accs stripe =
    [ XXH32_round accN (toLE laneN) | accN <- accs | laneN <- split stripe ]

  /**
   * This function combines the four lane accumulators into a single
   * 32-bit value.
   */
  XXH32_converge : [4][32] -> [32]
  XXH32_converge [acc1, acc2, acc3, acc4] =
    (acc1 <<< 1) + (acc2 <<< 7) + (acc3 <<< 12) + (acc4 <<< 18)

  /**
   * This function digests a four byte lane
   */
  XXH32_digest4 : [32] -> [4][8] -> [32]
  XXH32_digest4 acc lane = ((acc + toLE lane * PRIME32_3) <<< 17) * PRIME32_4

  /**
   * This function digests a single byte lane
   */
  XXH32_digest1 : [32] -> [8] -> [32]
  XXH32_digest1 acc lane = ((acc + (0 # lane) * PRIME32_5) <<< 11) * PRIME32_1

  /**
   * This function ensures that all input bits have a chance to impact
   * any bit in the output digest, resulting in an unbiased
   * distribution.
   */
  XXH32_avalanche : [32] -> [32]
  XXH32_avalanche acc0 = acc5
    where acc1 = acc0 ^ (acc0 >> 15)
          acc2 = acc1 * PRIME32_2
          acc3 = acc2 ^ (acc2 >> 13)
          acc4 = acc3 * PRIME32_3
          acc5 = acc4 ^ (acc4 >> 16)

  //64-bit xxHash helper functions

  //64-bit prime number constants
  PRIME64_1 = 0x9E3779B185EBCA87 : [64]
  PRIME64_2 = 0xC2B2AE3D27D4EB4F : [64]
  PRIME64_3 = 0x165667B19E3779F9 : [64]
  PRIME64_4 = 0x85EBCA77C2B2AE63 : [64]
  PRIME64_5 = 0x27D4EB2F165667C5 : [64]

  /**
   * The property shows that the hexadecimal representation of the
   * PRIME64 constants is the same as the binary representation.
   */
  property PRIME64s_as_bits_correct =
    (PRIME64_1 == 0b1001111000110111011110011011000110000101111010111100101010000111) /\
    (PRIME64_2 == 0b1100001010110010101011100011110100100111110101001110101101001111) /\
    (PRIME64_3 == 0b0001011001010110011001111011000110011110001101110111100111111001) /\
    (PRIME64_4 == 0b1000010111101011110010100111011111000010101100101010111001100011) /\
    (PRIME64_5 == 0b0010011111010100111010110010111100010110010101100110011111000101)

  /**
   * This function initializes the four internal accumulators of XXH64.
   */
  XXH64_init : [64] -> [4][64]
  XXH64_init seed = [acc1, acc2, acc3, acc4]
    where acc1 = seed + PRIME64_1 + PRIME64_2
          acc2 = seed + PRIME64_2
          acc3 = seed + 0
          acc4 = seed - PRIME64_1

  /**
   * This processes a single lane of the main round function of XXH64.
   */
  XXH64_round : [64] -> [64] -> [64]
  XXH64_round accN laneN = ((accN + laneN * PRIME64_2) <<< 31) * PRIME64_1

  /**
   * This is the main round function of XXH64 and processes a stripe,
   * i.e. 4 lanes with 8 bytes each.
   */
  XXH64_rounds : [4][64] -> [32][8] -> [4][64]
  XXH64_rounds accs stripe =
    [ XXH64_round accN (toLE laneN) | accN <- accs | laneN <- split stripe ]

  /**
   * This is a helper function, used to merge the four lane accumulators.
   */
  mergeAccumulator : [64] -> [64] -> [64]
  mergeAccumulator acc accN = (acc ^ XXH64_round 0 accN) * PRIME64_1 + PRIME64_4

  /**
   * This function combines the four lane accumulators into a single
   * 64-bit value.
   */
  XXH64_converge : [4][64] -> [64]
  XXH64_converge [acc1, acc2, acc3, acc4] =
    foldl mergeAccumulator ((acc1 <<< 1) + (acc2 <<< 7) + (acc3 <<< 12) + (acc4 <<< 18)) [acc1, acc2, acc3, acc4]

  /**
   * This function digests an eight byte lane
   */
  XXH64_digest8 : [64] -> [8][8] -> [64]
  XXH64_digest8 acc lane = ((acc ^ XXH64_round 0 (toLE lane)) <<< 27) * PRIME64_1 + PRIME64_4

  /**
   * This function digests a four byte lane
   */
  XXH64_digest4 : [64] -> [4][8] -> [64]
  XXH64_digest4 acc lane = ((acc ^ (0 # toLE lane) * PRIME64_1) <<< 23) * PRIME64_2 + PRIME64_3

  /**
   * This function digests a single byte lane
   */
  XXH64_digest1 : [64] -> [8] -> [64]
  XXH64_digest1 acc lane = ((acc ^ (0 # lane) * PRIME64_5) <<< 11) * PRIME64_1

  /**
   * This function ensures that all input bits have a chance to impact
   * any bit in the output digest, resulting in an unbiased
   * distribution.
   */
  XXH64_avalanche : [64] -> [64]
  XXH64_avalanche acc0 = acc5
    where acc1 = acc0 ^ (acc0 >> 33)
          acc2 = acc1 * PRIME64_2
          acc3 = acc2 ^ (acc2 >> 29)
          acc4 = acc3 * PRIME64_3
          acc5 = acc4 ^ (acc4 >> 32)
