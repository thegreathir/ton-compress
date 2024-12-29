#include <stddef.h>
#include <stdint.h>
int tinyLzmaCompress(const uint8_t *p_src, size_t src_len, uint8_t *p_dst,
                     size_t *p_dst_len);
#define R_OK 0
#define R_ERR_MEMORY_RUNOUT 1
#define R_ERR_UNSUPPORTED 2
#define R_ERR_OUTPUT_OVERFLOW 3
#include <stdlib.h>
#define RET_IF_ERROR(expression)                                               \
  {                                                                            \
    int res = (expression);                                                    \
    if (res != R_OK)                                                           \
      return res;                                                              \
  }
static uint32_t bitsReverse(uint32_t bits, uint32_t bit_count) {
  uint32_t revbits = 0;
  for (; bit_count > 0; bit_count--) {
    revbits <<= 1;
    revbits |= (bits & 1);
    bits >>= 1;
  }
  return revbits;
}
static uint32_t countBit(uint32_t val) {
  uint32_t count = 0;
  for (; val != 0; val >>= 1)
    count++;
  return count;
}
#define RANGE_CODE_NORMALIZE_THRESHOLD (1 << 24)
#define RANGE_CODE_MOVE_BITS 5
#define RANGE_CODE_N_BIT_MODEL_TOTAL_BITS 11
#define RANGE_CODE_BIT_MODEL_TOTAL (1 << RANGE_CODE_N_BIT_MODEL_TOTAL_BITS)
#define RANGE_CODE_HALF_PROBABILITY (RANGE_CODE_BIT_MODEL_TOTAL >> 1)
#define RANGE_CODE_CACHE_SIZE_MAX (~((size_t)0))
typedef struct {
  uint8_t overflow;
  uint8_t cache;
  uint8_t low_msb;
  uint32_t low_lsb;
  uint32_t range;
  size_t cache_size;
  uint8_t *p_dst;
  uint8_t *p_dst_limit;
} RangeEncoder_t;
static RangeEncoder_t newRangeEncoder(uint8_t *p_dst, size_t dst_len) {
  RangeEncoder_t coder;
  coder.cache = 0;
  coder.low_msb = 0;
  coder.low_lsb = 0;
  coder.range = 0xFFFFFFFF;
  coder.cache_size = 1;
  coder.p_dst = p_dst;
  coder.p_dst_limit = p_dst + dst_len;
  coder.overflow = 0;
  return coder;
}
static void rangeEncodeOutByte(RangeEncoder_t *e, uint8_t byte) {
  if (e->p_dst != e->p_dst_limit)
    *(e->p_dst++) = byte;
  else
    e->overflow = 1;
}
static void rangeEncodeNormalize(RangeEncoder_t *e) {
  if (e->range < RANGE_CODE_NORMALIZE_THRESHOLD) {
    if (e->low_msb) {
      rangeEncodeOutByte(e, e->cache + 1);
      for (; e->cache_size > 1; e->cache_size--)
        rangeEncodeOutByte(e, 0x00);
      e->cache = (uint8_t)((e->low_lsb) >> 24);
      e->cache_size = 0;
    } else if (e->low_lsb < 0xFF000000) {
      rangeEncodeOutByte(e, e->cache);
      for (; e->cache_size > 1; e->cache_size--)
        rangeEncodeOutByte(e, 0xFF);
      e->cache = (uint8_t)((e->low_lsb) >> 24);
      e->cache_size = 0;
    }
    if (e->cache_size < RANGE_CODE_CACHE_SIZE_MAX)
      e->cache_size++;
    e->low_msb = 0;
    e->low_lsb <<= 8;
    e->range <<= 8;
  }
}
static void rangeEncodeTerminate(RangeEncoder_t *e) {
  e->range = 0;
  rangeEncodeNormalize(e);
  rangeEncodeNormalize(e);
  rangeEncodeNormalize(e);
  rangeEncodeNormalize(e);
  rangeEncodeNormalize(e);
  rangeEncodeNormalize(e);
}
static void rangeEncodeIntByFixedProb(RangeEncoder_t *e, uint32_t val,
                                      uint32_t bit_count) {
  for (; bit_count > 0; bit_count--) {
    uint8_t bit = 1 & (val >> (bit_count - 1));
    rangeEncodeNormalize(e);
    e->range >>= 1;
    if (bit) {
      if ((e->low_lsb + e->range) < e->low_lsb)
        e->low_msb = 1;
      e->low_lsb += e->range;
    }
  }
}
static void rangeEncodeBit(RangeEncoder_t *e, uint16_t *p_prob, uint8_t bit) {
  uint32_t prob = *p_prob;
  uint32_t bound;
  rangeEncodeNormalize(e);
  bound = (e->range >> RANGE_CODE_N_BIT_MODEL_TOTAL_BITS) * prob;
  if (!bit) {
    e->range = bound;
    *p_prob = (uint16_t)(prob + ((RANGE_CODE_BIT_MODEL_TOTAL - prob) >>
                                 RANGE_CODE_MOVE_BITS));
  } else {
    e->range -= bound;
    if ((e->low_lsb + bound) < e->low_lsb)
      e->low_msb = 1;
    e->low_lsb += bound;
    *p_prob = (uint16_t)(prob - (prob >> RANGE_CODE_MOVE_BITS));
  }
}
static void rangeEncodeInt(RangeEncoder_t *e, uint16_t *p_prob, uint32_t val,
                           uint32_t bit_count) {
  uint32_t treepos = 1;
  for (; bit_count > 0; bit_count--) {
    uint8_t bit = (uint8_t)(1 & (val >> (bit_count - 1)));
    rangeEncodeBit(e, p_prob + (treepos - 1), bit);
    treepos <<= 1;
    if (bit)
      treepos |= 1;
  }
}
static void rangeEncodeMB(RangeEncoder_t *e, uint16_t *p_prob, uint32_t byte,
                          uint32_t match_byte) {
  uint32_t i, treepos = 1, off0 = 0x100, off1;
  for (i = 0; i < 8; i++) {
    uint8_t bit = (uint8_t)(1 & (byte >> 7));
    byte <<= 1;
    match_byte <<= 1;
    off1 = off0;
    off0 &= match_byte;
    rangeEncodeBit(e, p_prob + (off0 + off1 + treepos - 1), bit);
    treepos <<= 1;
    if (bit)
      treepos |= 1;
    else
      off0 ^= off1;
  }
}
#define LZ_LEN_MAX 273
#define LZ_DIST_MAX_PLUS1 0x40000000
#define HASH_LEVEL 2
#define HASH_N 23
#define HASH_SIZE (1 << HASH_N)
#define HASH_MASK ((1 << HASH_N) - 1)
#define INVALID_HASH_ITEM (~((size_t)0))
#define INIT_HASH_TABLE(hash_table)                                            \
  {                                                                            \
    uint32_t i, j;                                                             \
    for (i = 0; i < HASH_SIZE; i++)                                            \
      for (j = 0; j < HASH_LEVEL; j++)                                         \
        hash_table[i][j] = INVALID_HASH_ITEM;                                  \
  }
static uint32_t getHash(const uint8_t *p_src, size_t src_len, size_t pos) {
  if (pos >= src_len || pos + 1 == src_len || pos + 2 == src_len)
    return 0;
  else
#if HASH_N < 24
    return ((p_src[pos + 2] << 16) + (p_src[pos + 1] << 8) + p_src[pos]) &
           HASH_MASK;
#else
    return ((p_src[pos + 2] << 16) + (p_src[pos + 1] << 8) + p_src[pos]);
#endif
}
static void updateHashTable(const uint8_t *p_src, size_t src_len, size_t pos,
                            size_t hash_table[][HASH_LEVEL]) {
  const uint32_t hash = getHash(p_src, src_len, pos);
  uint32_t i, oldest_i = 0;
  size_t oldest_pos = INVALID_HASH_ITEM;
  if (pos >= src_len)
    return;
  for (i = 0; i < HASH_LEVEL; i++) {
    if (hash_table[hash][i] == INVALID_HASH_ITEM) {
      hash_table[hash][i] = pos;
      return;
    }
    if (oldest_pos > hash_table[hash][i]) {
      oldest_pos = hash_table[hash][i];
      oldest_i = i;
    }
  }
  hash_table[hash][oldest_i] = pos;
}
static uint32_t lenDistScore(uint32_t len, uint32_t dist, uint32_t rep0,
                             uint32_t rep1, uint32_t rep2, uint32_t rep3) {
  static const uint32_t TABLE_THRESHOLDS[] = {
      12 * 12 * 12 * 12 * 12 * 5, 12 * 12 * 12 * 12 * 4, 12 * 12 * 12 * 3,
      12 * 12 * 2, 12};
  uint32_t score;
  if (dist == rep0 || dist == rep1 || dist == rep2 || dist == rep3) {
    score = 5;
  } else {
    for (score = 4; score > 0; score--)
      if (dist <= TABLE_THRESHOLDS[score])
        break;
  }
  if (len < 2)
    return 8 + 5;
  else if (len == 2)
    return 8 + score + 1;
  else
    return 8 + score + len;
}
static void lzSearchMatch(const uint8_t *p_src, size_t src_len, size_t pos,
                          size_t hash_table[][HASH_LEVEL], uint32_t *p_len,
                          uint32_t *p_dist) {
  const uint32_t len_max =
      ((src_len - pos) < LZ_LEN_MAX) ? (src_len - pos) : LZ_LEN_MAX;
  const uint32_t hash = getHash(p_src, src_len, pos);
  uint32_t i, j, score1, score2;
  *p_len = 0;
  *p_dist = 0;
  score1 = lenDistScore(0, 0xFFFFFFFF, 0, 0, 0, 0);
  for (i = 0; i < HASH_LEVEL + 2; i++) {
    size_t ppos =
        (i < HASH_LEVEL) ? hash_table[hash][i] : (pos - 1 - (i - HASH_LEVEL));
    if (ppos != INVALID_HASH_ITEM && ppos < pos &&
        (pos - ppos) < LZ_DIST_MAX_PLUS1) {
      for (j = 0; j < len_max; j++)
        if (p_src[pos + j] != p_src[ppos + j])
          break;
      score2 = lenDistScore(j, (pos - ppos), 0, 0, 0, 0);
      if (j >= 2 && score1 < score2) {
        score1 = score2;
        *p_len = j;
        *p_dist = pos - ppos;
      }
    }
  }
}
static void lzSearchRep(const uint8_t *p_src, size_t src_len, size_t pos,
                        uint32_t rep0, uint32_t rep1, uint32_t rep2,
                        uint32_t rep3, uint32_t len_limit, uint32_t *p_len,
                        uint32_t *p_dist) {
  uint32_t len_max =
      ((src_len - pos) < LZ_LEN_MAX) ? (src_len - pos) : LZ_LEN_MAX;
  uint32_t reps[4];
  uint32_t i, j;
  if (len_max > len_limit)
    len_max = len_limit;
  reps[0] = rep0;
  reps[1] = rep1;
  reps[2] = rep2;
  reps[3] = rep3;
  *p_len = 0;
  *p_dist = 0;
  for (i = 0; i < 4; i++) {
    if (reps[i] <= pos) {
      size_t ppos = pos - reps[i];
      for (j = 0; j < len_max; j++)
        if (p_src[pos + j] != p_src[ppos + j])
          break;
      if (j >= 2 && j > *p_len) {
        *p_len = j;
        *p_dist = reps[i];
      }
    }
  }
}
static void lzSearch(const uint8_t *p_src, size_t src_len, size_t pos,
                     uint32_t rep0, uint32_t rep1, uint32_t rep2, uint32_t rep3,
                     size_t hash_table[][HASH_LEVEL], uint32_t *p_len,
                     uint32_t *p_dist) {
  uint32_t rlen, rdist;
  uint32_t mlen, mdist;
  lzSearchRep(p_src, src_len, pos, rep0, rep1, rep2, rep3, 0xFFFFFFFF, &rlen,
              &rdist);
  lzSearchMatch(p_src, src_len, pos, hash_table, &mlen, &mdist);
  if (lenDistScore(rlen, rdist, rep0, rep1, rep2, rep3) >=
      lenDistScore(mlen, mdist, rep0, rep1, rep2, rep3)) {
    *p_len = rlen;
    *p_dist = rdist;
  } else {
    *p_len = mlen;
    *p_dist = mdist;
  }
}
static uint8_t isShortRep(const uint8_t *p_src, size_t src_len, size_t pos,
                          uint32_t rep0) {
  return (pos >= rep0 && (p_src[pos] == p_src[pos - rep0])) ? 1 : 0;
}
typedef enum {
  PKT_LIT,
  PKT_MATCH,
  PKT_SHORTREP,
  PKT_REP0,
  PKT_REP1,
  PKT_REP2,
  PKT_REP3
} PACKET_t;
static uint8_t stateTransition(uint8_t state, PACKET_t type) {
  switch (state) {
  case 0:
    return (type == PKT_LIT)        ? 0
           : (type == PKT_MATCH)    ? 7
           : (type == PKT_SHORTREP) ? 9
                                    : 8;
  case 1:
    return (type == PKT_LIT)        ? 0
           : (type == PKT_MATCH)    ? 7
           : (type == PKT_SHORTREP) ? 9
                                    : 8;
  case 2:
    return (type == PKT_LIT)        ? 0
           : (type == PKT_MATCH)    ? 7
           : (type == PKT_SHORTREP) ? 9
                                    : 8;
  case 3:
    return (type == PKT_LIT)        ? 0
           : (type == PKT_MATCH)    ? 7
           : (type == PKT_SHORTREP) ? 9
                                    : 8;
  case 4:
    return (type == PKT_LIT)        ? 1
           : (type == PKT_MATCH)    ? 7
           : (type == PKT_SHORTREP) ? 9
                                    : 8;
  case 5:
    return (type == PKT_LIT)        ? 2
           : (type == PKT_MATCH)    ? 7
           : (type == PKT_SHORTREP) ? 9
                                    : 8;
  case 6:
    return (type == PKT_LIT)        ? 3
           : (type == PKT_MATCH)    ? 7
           : (type == PKT_SHORTREP) ? 9
                                    : 8;
  case 7:
    return (type == PKT_LIT)        ? 4
           : (type == PKT_MATCH)    ? 10
           : (type == PKT_SHORTREP) ? 11
                                    : 11;
  case 8:
    return (type == PKT_LIT)        ? 5
           : (type == PKT_MATCH)    ? 10
           : (type == PKT_SHORTREP) ? 11
                                    : 11;
  case 9:
    return (type == PKT_LIT)        ? 6
           : (type == PKT_MATCH)    ? 10
           : (type == PKT_SHORTREP) ? 11
                                    : 11;
  case 10:
    return (type == PKT_LIT)        ? 4
           : (type == PKT_MATCH)    ? 10
           : (type == PKT_SHORTREP) ? 11
                                    : 11;
  case 11:
    return (type == PKT_LIT)        ? 5
           : (type == PKT_MATCH)    ? 10
           : (type == PKT_SHORTREP) ? 11
                                    : 11;
  default:
    return 0xFF;
  }
}
#define N_STATES 12
#define N_LIT_STATES 7
#define LC 8
#define N_PREV_BYTE_LC_MSBS (1 << LC)
#define LC_SHIFT (8 - LC)
#define LC_MASK ((1 << LC) - 1)
#define LP_PARAM 0
#define N_LIT_POS_STATES (1 << LP_PARAM)
#define LP_MASK ((1 << LP_PARAM) - 1)
#define PB 0
#define N_POS_STATES (1 << PB)
#define PB_MASK ((1 << PB) - 1)
#define LCLPPB_BYTE ((uint8_t)((PB * 5 + LP_PARAM) * 9 + LC))
#define INIT_PROBS(probs)                                                      \
  {                                                                            \
    uint16_t *p = (uint16_t *)(probs);                                         \
    uint16_t *q = p + (sizeof(probs) / sizeof(uint16_t));                      \
    for (; p < q; p++)                                                         \
      *p = RANGE_CODE_HALF_PROBABILITY;                                        \
  }
static int lzmaEncode(const uint8_t *p_src, size_t src_len, uint8_t *p_dst,
                      size_t *p_dst_len, uint8_t with_end_mark) {
  uint8_t state = 0;
  size_t pos = 0;
  uint32_t rep0 = 1;
  uint32_t rep1 = 1;
  uint32_t rep2 = 1;
  uint32_t rep3 = 1;
  uint32_t n_bypass = 0, len_bypass = 0, dist_bypass = 0;
  RangeEncoder_t coder = newRangeEncoder(p_dst, *p_dst_len);
  uint16_t probs_is_match[N_STATES][N_POS_STATES];
  uint16_t probs_is_rep[N_STATES];
  uint16_t probs_is_rep0[N_STATES];
  uint16_t probs_is_rep0_long[N_STATES][N_POS_STATES];
  uint16_t probs_is_rep1[N_STATES];
  uint16_t probs_is_rep2[N_STATES];
  uint16_t probs_literal[N_LIT_POS_STATES][N_PREV_BYTE_LC_MSBS][3 * (1 << 8)];
  uint16_t probs_dist_slot[4][(1 << 6) - 1];
  uint16_t probs_dist_special[10][(1 << 5) - 1];
  uint16_t probs_dist_align[(1 << 4) - 1];
  uint16_t probs_len_choice[2];
  uint16_t probs_len_choice2[2];
  uint16_t probs_len_low[2][N_POS_STATES][(1 << 3) - 1];
  uint16_t probs_len_mid[2][N_POS_STATES][(1 << 3) - 1];
  uint16_t probs_len_high[2][(1 << 8) - 1];
  size_t(*hash_table)[HASH_LEVEL];
  hash_table =
      (size_t(*)[HASH_LEVEL])malloc(sizeof(size_t) * HASH_SIZE * HASH_LEVEL);
  if (hash_table == 0)
    return R_ERR_MEMORY_RUNOUT;
  INIT_HASH_TABLE(hash_table);
  INIT_PROBS(probs_is_match);
  INIT_PROBS(probs_is_rep);
  INIT_PROBS(probs_is_rep0);
  INIT_PROBS(probs_is_rep0_long);
  INIT_PROBS(probs_is_rep1);
  INIT_PROBS(probs_is_rep2);
  INIT_PROBS(probs_literal);
  INIT_PROBS(probs_dist_slot);
  INIT_PROBS(probs_dist_special);
  INIT_PROBS(probs_dist_align);
  INIT_PROBS(probs_len_choice);
  INIT_PROBS(probs_len_choice2);
  INIT_PROBS(probs_len_low);
  INIT_PROBS(probs_len_mid);
  INIT_PROBS(probs_len_high);
  while (!coder.overflow) {
    const uint32_t lit_pos_state = LP_MASK & (uint32_t)pos;
    const uint32_t pos_state = PB_MASK & (uint32_t)pos;
    uint32_t curr_byte = 0, match_byte = 0, prev_byte_lc_msbs = 0;
    uint32_t dist = 0, len = 0;
    PACKET_t type;
    if (pos < src_len)
      curr_byte = p_src[pos];
    if (pos > 0) {
      match_byte = p_src[pos - rep0];
      prev_byte_lc_msbs = (p_src[pos - 1] >> LC_SHIFT) & LC_MASK;
    }
    if (pos >= src_len) {
      if (!with_end_mark)
        break;
      with_end_mark = 0;
      type = PKT_MATCH;
      len = 2;
      dist = 0;
    } else {
      if (n_bypass > 0) {
        len = 0;
        dist = 0;
        n_bypass--;
      } else if (len_bypass > 0) {
        len = len_bypass;
        dist = dist_bypass;
        len_bypass = 0;
        dist_bypass = 0;
      } else {
        lzSearch(p_src, src_len, pos, rep0, rep1, rep2, rep3, hash_table, &len,
                 &dist);
        if ((src_len - pos) > 8 && len >= 2) {
          const uint32_t score0 =
              lenDistScore(len, dist, rep0, rep1, rep2, rep3);
          uint32_t len1 = 0, dist1 = 0, score1 = 0;
          uint32_t len2 = 0, dist2 = 0, score2 = 0;
          lzSearch(p_src, src_len, pos + 1, rep0, rep1, rep2, rep3, hash_table,
                   &len1, &dist1);
          score1 = lenDistScore(len1, dist1, rep0, rep1, rep2, rep3);
          if (len >= 3) {
            lzSearch(p_src, src_len, pos + 2, rep0, rep1, rep2, rep3,
                     hash_table, &len2, &dist2);
            score2 = lenDistScore(len2, dist2, rep0, rep1, rep2, rep3) - 1;
          }
          if (score2 > score0 && score2 > score1) {
            len = 0;
            dist = 0;
            lzSearchRep(p_src, src_len, pos, rep0, rep1, rep2, rep3, 2, &len,
                        &dist);
            len_bypass = len2;
            dist_bypass = dist2;
            n_bypass = (len < 2) ? 1 : 0;
          } else if (score1 > score0) {
            len = 0;
            dist = 0;
            len_bypass = len1;
            dist_bypass = dist1;
            n_bypass = 0;
          }
        }
      }
      if (len < 2) {
        type = isShortRep(p_src, src_len, pos, rep0) ? PKT_SHORTREP : PKT_LIT;
      } else if (dist == rep0) {
        type = PKT_REP0;
      } else if (dist == rep1) {
        type = PKT_REP1;
        rep1 = rep0;
        rep0 = dist;
      } else if (dist == rep2) {
        type = PKT_REP2;
        rep2 = rep1;
        rep1 = rep0;
        rep0 = dist;
      } else if (dist == rep3) {
        type = PKT_REP3;
        rep3 = rep2;
        rep2 = rep1;
        rep1 = rep0;
        rep0 = dist;
      } else {
        type = PKT_MATCH;
        rep3 = rep2;
        rep2 = rep1;
        rep1 = rep0;
        rep0 = dist;
      }
      {
        const size_t pos2 =
            pos + ((type == PKT_LIT || type == PKT_SHORTREP) ? 1 : len);
        for (; pos < pos2; pos++)
          updateHashTable(p_src, src_len, pos, hash_table);
      }
    }
    switch (type) {
    case PKT_LIT:
      rangeEncodeBit(&coder, &probs_is_match[state][pos_state], 0);
      break;
    case PKT_MATCH:
      rangeEncodeBit(&coder, &probs_is_match[state][pos_state], 1);
      rangeEncodeBit(&coder, &probs_is_rep[state], 0);
      break;
    case PKT_SHORTREP:
      rangeEncodeBit(&coder, &probs_is_match[state][pos_state], 1);
      rangeEncodeBit(&coder, &probs_is_rep[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep0[state], 0);
      rangeEncodeBit(&coder, &probs_is_rep0_long[state][pos_state], 0);
      break;
    case PKT_REP0:
      rangeEncodeBit(&coder, &probs_is_match[state][pos_state], 1);
      rangeEncodeBit(&coder, &probs_is_rep[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep0[state], 0);
      rangeEncodeBit(&coder, &probs_is_rep0_long[state][pos_state], 1);
      break;
    case PKT_REP1:
      rangeEncodeBit(&coder, &probs_is_match[state][pos_state], 1);
      rangeEncodeBit(&coder, &probs_is_rep[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep0[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep1[state], 0);
      break;
    case PKT_REP2:
      rangeEncodeBit(&coder, &probs_is_match[state][pos_state], 1);
      rangeEncodeBit(&coder, &probs_is_rep[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep0[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep1[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep2[state], 0);
      break;
    default:
      rangeEncodeBit(&coder, &probs_is_match[state][pos_state], 1);
      rangeEncodeBit(&coder, &probs_is_rep[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep0[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep1[state], 1);
      rangeEncodeBit(&coder, &probs_is_rep2[state], 1);
      break;
    }
    if (type == PKT_LIT) {
      if (state < N_LIT_STATES)
        rangeEncodeInt(&coder, probs_literal[lit_pos_state][prev_byte_lc_msbs],
                       curr_byte, 8);
      else
        rangeEncodeMB(&coder, probs_literal[lit_pos_state][prev_byte_lc_msbs],
                      curr_byte, match_byte);
    }
    if (type == PKT_MATCH || type == PKT_REP0 || type == PKT_REP1 ||
        type == PKT_REP2 || type == PKT_REP3) {
      const uint8_t isrep = (type != PKT_MATCH);
      if (len < 10) {
        rangeEncodeBit(&coder, &probs_len_choice[isrep], 0);
        rangeEncodeInt(&coder, probs_len_low[isrep][pos_state], len - 2, 3);
      } else if (len < 18) {
        rangeEncodeBit(&coder, &probs_len_choice[isrep], 1);
        rangeEncodeBit(&coder, &probs_len_choice2[isrep], 0);
        rangeEncodeInt(&coder, probs_len_mid[isrep][pos_state], len - 10, 3);
      } else {
        rangeEncodeBit(&coder, &probs_len_choice[isrep], 1);
        rangeEncodeBit(&coder, &probs_len_choice2[isrep], 1);
        rangeEncodeInt(&coder, probs_len_high[isrep], len - 18, 8);
      }
    }
    if (type == PKT_MATCH) {
      const uint32_t len_min5_minus2 = (len > 5) ? 3 : (len - 2);
      uint32_t dist_slot, bcnt, bits;
      dist--;
      if (dist < 4) {
        dist_slot = dist;
      } else {
        dist_slot = countBit(dist) - 1;
        dist_slot = (dist_slot << 1) | ((dist >> (dist_slot - 1)) & 1);
      }
      rangeEncodeInt(&coder, probs_dist_slot[len_min5_minus2], dist_slot, 6);
      bcnt = (dist_slot >> 1) - 1;
      if (dist_slot >= 14) {
        bcnt -= 4;
        bits = (dist >> 4) & ((1 << bcnt) - 1);
        rangeEncodeIntByFixedProb(&coder, bits, bcnt);
        bits = dist & ((1 << 4) - 1);
        bits = bitsReverse(bits, 4);
        rangeEncodeInt(&coder, probs_dist_align, bits, 4);
      } else if (dist_slot >= 4) {
        bits = dist & ((1 << bcnt) - 1);
        bits = bitsReverse(bits, bcnt);
        rangeEncodeInt(&coder, probs_dist_special[dist_slot - 4], bits, bcnt);
      }
    }
    state = stateTransition(state, type);
  }
  free(hash_table);
  rangeEncodeTerminate(&coder);
  if (coder.overflow)
    return R_ERR_OUTPUT_OVERFLOW;
  *p_dst_len = coder.p_dst - p_dst;
  return R_OK;
}
#define LZMA_DIC_MIN 4096
#define LZMA_DIC_LEN                                                           \
  ((LZ_DIST_MAX_PLUS1 > LZMA_DIC_MIN) ? LZ_DIST_MAX_PLUS1 : LZMA_DIC_MIN)
#define LZMA_HEADER_LEN 13
static int writeLzmaHeader(uint8_t *p_dst, size_t *p_dst_len,
                           size_t uncompressed_len,
                           uint8_t uncompressed_len_known) {
  uint32_t i;
  if (*p_dst_len < LZMA_HEADER_LEN)
    return R_ERR_OUTPUT_OVERFLOW;
  *p_dst_len = LZMA_HEADER_LEN;
  *(p_dst++) = LCLPPB_BYTE;
  for (i = 0; i < 4; i++)
    *(p_dst++) = (uint8_t)(LZMA_DIC_LEN >> (i * 8));
  for (i = 0; i < 8; i++) {
    if (uncompressed_len_known) {
      *(p_dst++) = (uint8_t)uncompressed_len;
      uncompressed_len >>= 8;
    } else {
      *(p_dst++) = 0xFF;
    }
  }
  return R_OK;
}
int tinyLzmaCompress(const uint8_t *p_src, size_t src_len, uint8_t *p_dst,
                     size_t *p_dst_len) {
  size_t hdr_len, cmprs_len;
  hdr_len = *p_dst_len;
  RET_IF_ERROR(writeLzmaHeader(p_dst, &hdr_len, src_len, 1));
  cmprs_len = *p_dst_len - hdr_len;
  RET_IF_ERROR(lzmaEncode(p_src, src_len, p_dst + hdr_len, &cmprs_len, 0));
  *p_dst_len = hdr_len + cmprs_len;
  return R_OK;
}
static size_t getStringLength(const char *string) {
  size_t i;
  for (i = 0; *string; string++, i++)
    ;
  return i;
}
#define ZIP_LZMA_PROPERTY_LEN 9
#define ZIP_HEADER_LEN_EXCLUDE_FILENAME 30
#define ZIP_FOOTER_LEN_EXCLUDE_FILENAME (46 + 22)
#define FILE_NAME_IN_ZIP_MAX_LEN ((size_t)0xFF00)
#define ZIP_UNCOMPRESSED_MAX_LEN ((size_t)0xFFFF0000)
#define ZIP_COMPRESSED_MAX_LEN ((size_t)0xFFFF0000)
static int writeZipHeader(uint8_t *p_dst, size_t *p_dst_len, uint32_t crc,
                          size_t compressed_len, size_t uncompressed_len,
                          const char *file_name) {
  size_t i;
  const size_t file_name_len = getStringLength(file_name);
  if (file_name_len > FILE_NAME_IN_ZIP_MAX_LEN)
    return R_ERR_UNSUPPORTED;
  if (uncompressed_len > ZIP_UNCOMPRESSED_MAX_LEN)
    return R_ERR_UNSUPPORTED;
  if (compressed_len > ZIP_COMPRESSED_MAX_LEN)
    return R_ERR_UNSUPPORTED;
  if (*p_dst_len < ZIP_HEADER_LEN_EXCLUDE_FILENAME + file_name_len)
    return R_ERR_OUTPUT_OVERFLOW;
  *p_dst_len = ZIP_HEADER_LEN_EXCLUDE_FILENAME + file_name_len;
  *(p_dst++) = 0x50;
  *(p_dst++) = 0x4B;
  *(p_dst++) = 0x03;
  *(p_dst++) = 0x04;
  *(p_dst++) = 0x3F;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x0E;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = (uint8_t)(crc >> 0);
  *(p_dst++) = (uint8_t)(crc >> 8);
  *(p_dst++) = (uint8_t)(crc >> 16);
  *(p_dst++) = (uint8_t)(crc >> 24);
  *(p_dst++) = (uint8_t)(compressed_len >> 0);
  *(p_dst++) = (uint8_t)(compressed_len >> 8);
  *(p_dst++) = (uint8_t)(compressed_len >> 16);
  *(p_dst++) = (uint8_t)(compressed_len >> 24);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 0);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 8);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 16);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 24);
  *(p_dst++) = (uint8_t)(file_name_len >> 0);
  *(p_dst++) = (uint8_t)(file_name_len >> 8);
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  for (i = 0; i < file_name_len; i++)
    *(p_dst++) = file_name[i];
  return R_OK;
}
static int writeZipLzmaProperty(uint8_t *p_dst, size_t *p_dst_len) {
  if (*p_dst_len < ZIP_LZMA_PROPERTY_LEN)
    return R_ERR_OUTPUT_OVERFLOW;
  *p_dst_len = ZIP_LZMA_PROPERTY_LEN;
  *(p_dst++) = 0x10;
  *(p_dst++) = 0x02;
  *(p_dst++) = 0x05;
  *(p_dst++) = 0x00;
  *(p_dst++) = LCLPPB_BYTE;
  *(p_dst++) = (uint8_t)(LZMA_DIC_LEN >> 0);
  *(p_dst++) = (uint8_t)(LZMA_DIC_LEN >> 8);
  *(p_dst++) = (uint8_t)(LZMA_DIC_LEN >> 16);
  *(p_dst++) = (uint8_t)(LZMA_DIC_LEN >> 24);
  return R_OK;
}
static int writeZipFooter(uint8_t *p_dst, size_t *p_dst_len, uint32_t crc,
                          size_t compressed_len, size_t uncompressed_len,
                          const char *file_name, size_t offset) {
  size_t i;
  const size_t file_name_len = getStringLength(file_name);
  if (*p_dst_len < ZIP_FOOTER_LEN_EXCLUDE_FILENAME + file_name_len)
    return R_ERR_OUTPUT_OVERFLOW;
  *p_dst_len = ZIP_FOOTER_LEN_EXCLUDE_FILENAME + file_name_len;
  *(p_dst++) = 0x50;
  *(p_dst++) = 0x4B;
  *(p_dst++) = 0x01;
  *(p_dst++) = 0x02;
  *(p_dst++) = 0x1E;
  *(p_dst++) = 0x03;
  *(p_dst++) = 0x3F;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x0E;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = (uint8_t)(crc >> 0);
  *(p_dst++) = (uint8_t)(crc >> 8);
  *(p_dst++) = (uint8_t)(crc >> 16);
  *(p_dst++) = (uint8_t)(crc >> 24);
  *(p_dst++) = (uint8_t)(compressed_len >> 0);
  *(p_dst++) = (uint8_t)(compressed_len >> 8);
  *(p_dst++) = (uint8_t)(compressed_len >> 16);
  *(p_dst++) = (uint8_t)(compressed_len >> 24);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 0);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 8);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 16);
  *(p_dst++) = (uint8_t)(uncompressed_len >> 24);
  *(p_dst++) = (uint8_t)(file_name_len >> 0);
  *(p_dst++) = (uint8_t)(file_name_len >> 8);
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  for (i = 0; i < file_name_len; i++)
    *(p_dst++) = file_name[i];
  *(p_dst++) = 0x50;
  *(p_dst++) = 0x4B;
  *(p_dst++) = 0x05;
  *(p_dst++) = 0x06;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x01;
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x01;
  *(p_dst++) = 0x00;
  *(p_dst++) = (uint8_t)((46 + file_name_len) >> 0);
  *(p_dst++) = (uint8_t)((46 + file_name_len) >> 8);
  *(p_dst++) = (uint8_t)((46 + file_name_len) >> 16);
  *(p_dst++) = (uint8_t)((46 + file_name_len) >> 24);
  *(p_dst++) = (uint8_t)(offset >> 0);
  *(p_dst++) = (uint8_t)(offset >> 8);
  *(p_dst++) = (uint8_t)(offset >> 16);
  *(p_dst++) = (uint8_t)(offset >> 24);
  *(p_dst++) = 0x00;
  *(p_dst++) = 0x00;
  return R_OK;
}
static uint32_t calcCrc32(const uint8_t *p_src, size_t src_len) {
  static const uint32_t TABLE_CRC32[] = {
      0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
      0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
      0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
  uint32_t crc = 0xFFFFFFFF;
  const uint8_t *p_end = p_src + src_len;
  for (; p_src < p_end; p_src++) {
    crc ^= *p_src;
    crc = TABLE_CRC32[crc & 0x0f] ^ (crc >> 4);
    crc = TABLE_CRC32[crc & 0x0f] ^ (crc >> 4);
  }
  return ~crc;
}
#include <stddef.h>
#include <stdint.h>
int tinyLzmaDecompress(const uint8_t *p_src, size_t src_len, uint8_t *p_dst,
                       size_t *p_dst_len);
#define R_OK 0
#define R_ERR_MEMORY_RUNOUT 1
#define R_ERR_UNSUPPORTED 2
#define R_ERR_OUTPUT_OVERFLOW 3
#define R_ERR_INPUT_OVERFLOW 4
#define R_ERR_DATA 5
#define R_ERR_OUTPUT_LEN_MISMATCH 6
#include <stdlib.h>
#define RET_IF_ERROR(expression)                                               \
  {                                                                            \
    int res = (expression);                                                    \
    if (res != R_OK)                                                           \
      return res;                                                              \
  }
#define RANGE_CODE_NORMALIZE_THRESHOLD (1 << 24)
#define RANGE_CODE_MOVE_BITS 5
#define RANGE_CODE_N_BIT_MODEL_TOTAL_BITS 11
#define RANGE_CODE_BIT_MODEL_TOTAL (1 << RANGE_CODE_N_BIT_MODEL_TOTAL_BITS)
#define RANGE_CODE_HALF_PROBABILITY (RANGE_CODE_BIT_MODEL_TOTAL >> 1)
typedef struct {
  uint32_t code;
  uint32_t range;
  const uint8_t *p_src;
  const uint8_t *p_src_limit;
  uint8_t overflow;
} RangeDecoder_t;
static void rangeDecodeNormalize(RangeDecoder_t *d) {
  if (d->range < RANGE_CODE_NORMALIZE_THRESHOLD) {
    if (d->p_src != d->p_src_limit) {
      d->range <<= 8;
      d->code <<= 8;
      d->code |= (uint32_t)(*(d->p_src));
      d->p_src++;
    } else {
      d->overflow = 1;
    }
  }
}
static RangeDecoder_t newRangeDecoder(const uint8_t *p_src, size_t src_len) {
  RangeDecoder_t coder;
  coder.code = 0;
  coder.range = 0;
  coder.p_src = p_src;
  coder.p_src_limit = p_src + src_len;
  coder.overflow = 0;
  rangeDecodeNormalize(&coder);
  rangeDecodeNormalize(&coder);
  rangeDecodeNormalize(&coder);
  rangeDecodeNormalize(&coder);
  rangeDecodeNormalize(&coder);
  coder.range = 0xFFFFFFFF;
  return coder;
}
static uint32_t rangeDecodeIntByFixedProb(RangeDecoder_t *d,
                                          uint32_t bit_count) {
  uint32_t val = 0, b;
  for (; bit_count > 0; bit_count--) {
    rangeDecodeNormalize(d);
    d->range >>= 1;
    d->code -= d->range;
    b = !(1 & (d->code >> 31));
    if (!b)
      d->code += d->range;
    val <<= 1;
    val |= b;
  }
  return val;
}
static uint32_t rangeDecodeBit(RangeDecoder_t *d, uint16_t *p_prob) {
  uint32_t prob = *p_prob;
  uint32_t bound;
  rangeDecodeNormalize(d);
  bound = (d->range >> RANGE_CODE_N_BIT_MODEL_TOTAL_BITS) * prob;
  if (d->code < bound) {
    d->range = bound;
    *p_prob = (uint16_t)(prob + ((RANGE_CODE_BIT_MODEL_TOTAL - prob) >>
                                 RANGE_CODE_MOVE_BITS));
    return 0;
  } else {
    d->range -= bound;
    d->code -= bound;
    *p_prob = (uint16_t)(prob - (prob >> RANGE_CODE_MOVE_BITS));
    return 1;
  }
}
static uint32_t rangeDecodeInt(RangeDecoder_t *d, uint16_t *p_prob,
                               uint32_t bit_count) {
  uint32_t val = 1;
  uint32_t i;
  for (i = 0; i < bit_count; i++) {
    if (!rangeDecodeBit(d, p_prob + val - 1)) {
      val <<= 1;
    } else {
      val <<= 1;
      val |= 1;
    }
  }
  return val & ((1 << bit_count) - 1);
}
static uint32_t rangeDecodeMB(RangeDecoder_t *d, uint16_t *p_prob,
                              uint32_t match_byte) {
  uint32_t i, val = 1, off0 = 0x100, off1;
  for (i = 0; i < 8; i++) {
    match_byte <<= 1;
    off1 = off0;
    off0 &= match_byte;
    if (!rangeDecodeBit(d, (p_prob + (off0 + off1 + val - 1)))) {
      val <<= 1;
      off0 ^= off1;
    } else {
      val <<= 1;
      val |= 1;
    }
  }
  return val & 0xFF;
}
#define N_STATES 12
#define N_LIT_STATES 7
#define MAX_LC 8
#define N_PREV_BYTE_LC_MSBS (1 << MAX_LC)
#define MAX_LP 4
#define N_LIT_POS_STATES (1 << MAX_LP)
#define MAX_PB 4
#define N_POS_STATES (1 << MAX_PB)
#define INIT_PROBS(probs)                                                      \
  {                                                                            \
    uint16_t *p = (uint16_t *)(probs);                                         \
    uint16_t *q = p + (sizeof(probs) / sizeof(uint16_t));                      \
    for (; p < q; p++)                                                         \
      *p = RANGE_CODE_HALF_PROBABILITY;                                        \
  }
#define INIT_PROBS_LITERAL(probs)                                              \
  {                                                                            \
    uint16_t *p = (uint16_t *)(probs);                                         \
    uint16_t *q = p + (N_LIT_POS_STATES * N_PREV_BYTE_LC_MSBS * 3 * (1 << 8)); \
    for (; p < q; p++)                                                         \
      *p = RANGE_CODE_HALF_PROBABILITY;                                        \
  }
static int lzmaDecode(const uint8_t *p_src, size_t src_len, uint8_t *p_dst,
                      size_t *p_dst_len, uint8_t lc, uint8_t lp, uint8_t pb) {
  const uint8_t lc_shift = (8 - lc);
  const uint8_t lc_mask = (1 << lc) - 1;
  const uint8_t lp_mask = (1 << lp) - 1;
  const uint8_t pb_mask = (1 << pb) - 1;
  uint8_t prev_byte = 0;
  uint8_t state = 0;
  size_t pos = 0;
  uint32_t rep0 = 1;
  uint32_t rep1 = 1;
  uint32_t rep2 = 1;
  uint32_t rep3 = 1;
  RangeDecoder_t coder = newRangeDecoder(p_src, src_len);
  uint16_t probs_is_match[N_STATES][N_POS_STATES];
  uint16_t probs_is_rep[N_STATES];
  uint16_t probs_is_rep0[N_STATES];
  uint16_t probs_is_rep0_long[N_STATES][N_POS_STATES];
  uint16_t probs_is_rep1[N_STATES];
  uint16_t probs_is_rep2[N_STATES];
  uint16_t probs_dist_slot[4][(1 << 6) - 1];
  uint16_t probs_dist_special[10][(1 << 5) - 1];
  uint16_t probs_dist_align[(1 << 4) - 1];
  uint16_t probs_len_choice[2];
  uint16_t probs_len_choice2[2];
  uint16_t probs_len_low[2][N_POS_STATES][(1 << 3) - 1];
  uint16_t probs_len_mid[2][N_POS_STATES][(1 << 3) - 1];
  uint16_t probs_len_high[2][(1 << 8) - 1];
  uint16_t(*probs_literal)[N_PREV_BYTE_LC_MSBS][3 * (1 << 8)];
  probs_literal = (uint16_t(*)[N_PREV_BYTE_LC_MSBS][3 * (1 << 8)]) malloc(
      sizeof(uint16_t) * N_PREV_BYTE_LC_MSBS * N_LIT_POS_STATES * 3 * (1 << 8));
  if (probs_literal == 0)
    return R_ERR_MEMORY_RUNOUT;
  INIT_PROBS(probs_is_match);
  INIT_PROBS(probs_is_rep);
  INIT_PROBS(probs_is_rep0);
  INIT_PROBS(probs_is_rep0_long);
  INIT_PROBS(probs_is_rep1);
  INIT_PROBS(probs_is_rep2);
  INIT_PROBS(probs_dist_slot);
  INIT_PROBS(probs_dist_special);
  INIT_PROBS(probs_dist_align);
  INIT_PROBS(probs_len_choice);
  INIT_PROBS(probs_len_choice2);
  INIT_PROBS(probs_len_low);
  INIT_PROBS(probs_len_mid);
  INIT_PROBS(probs_len_high);
  INIT_PROBS_LITERAL(probs_literal);
  while (pos < *p_dst_len) {
    const uint8_t prev_byte_lc_msbs = lc_mask & (prev_byte >> lc_shift);
    const uint8_t literal_pos_state = lp_mask & (uint32_t)pos;
    const uint8_t pos_state = pb_mask & (uint32_t)pos;
    uint32_t dist = 0, len = 0;
    PACKET_t type;
    if (coder.overflow)
      return R_ERR_INPUT_OVERFLOW;
    if (!rangeDecodeBit(&coder, &probs_is_match[state][pos_state])) {
      type = PKT_LIT;
    } else if (!rangeDecodeBit(&coder, &probs_is_rep[state])) {
      type = PKT_MATCH;
    } else if (!rangeDecodeBit(&coder, &probs_is_rep0[state])) {
      type = rangeDecodeBit(&coder, &probs_is_rep0_long[state][pos_state])
                 ? PKT_REP0
                 : PKT_SHORTREP;
    } else if (!rangeDecodeBit(&coder, &probs_is_rep1[state])) {
      type = PKT_REP1;
    } else {
      type =
          rangeDecodeBit(&coder, &probs_is_rep2[state]) ? PKT_REP3 : PKT_REP2;
    }
    if (type == PKT_LIT) {
      if (state < N_LIT_STATES) {
        prev_byte = rangeDecodeInt(
            &coder, probs_literal[literal_pos_state][prev_byte_lc_msbs], 8);
      } else {
        uint8_t match_byte = 0;
        if (pos >= (size_t)rep0)
          match_byte = p_dst[pos - rep0];
        prev_byte = rangeDecodeMB(
            &coder, probs_literal[literal_pos_state][prev_byte_lc_msbs],
            match_byte);
      }
    }
    state = stateTransition(state, type);
    switch (type) {
    case PKT_SHORTREP:
    case PKT_REP0:
      dist = rep0;
      break;
    case PKT_REP1:
      dist = rep1;
      break;
    case PKT_REP2:
      dist = rep2;
      break;
    case PKT_REP3:
      dist = rep3;
      break;
    default:
      break;
    }
    switch (type) {
    case PKT_LIT:
    case PKT_SHORTREP:
      len = 1;
      break;
    case PKT_MATCH:
    case PKT_REP3:
      rep3 = rep2;
    case PKT_REP2:
      rep2 = rep1;
    case PKT_REP1:
      rep1 = rep0;
      break;
    default:
      break;
    }
    if (len == 0) {
      const uint32_t is_rep = (type != PKT_MATCH);
      if (!rangeDecodeBit(&coder, &probs_len_choice[is_rep]))
        len = 2 + rangeDecodeInt(&coder, probs_len_low[is_rep][pos_state], 3);
      else if (!rangeDecodeBit(&coder, &probs_len_choice2[is_rep]))
        len = 10 + rangeDecodeInt(&coder, probs_len_mid[is_rep][pos_state], 3);
      else
        len = 18 + rangeDecodeInt(&coder, probs_len_high[is_rep], 8);
    }
    if (type == PKT_MATCH) {
      const uint32_t len_min5_minus2 = (len > 5) ? 3 : (len - 2);
      uint32_t dist_slot, bcnt;
      dist_slot = rangeDecodeInt(&coder, probs_dist_slot[len_min5_minus2], 6);
      bcnt = (dist_slot >> 1) - 1;
      dist = (2 | (dist_slot & 1));
      dist <<= bcnt;
      if (dist_slot >= 14) {
        dist |= rangeDecodeIntByFixedProb(&coder, bcnt - 4) << 4;
        dist |= bitsReverse(rangeDecodeInt(&coder, probs_dist_align, 4), 4);
      } else if (dist_slot >= 4) {
        dist |= bitsReverse(
            rangeDecodeInt(&coder, probs_dist_special[dist_slot - 4], bcnt),
            bcnt);
      } else {
        dist = dist_slot;
      }
      if (dist == 0xFFFFFFFF)
        break;
      dist++;
    }
    if ((size_t)dist > pos)
      return R_ERR_DATA;
    if ((pos + len) > *p_dst_len)
      return R_ERR_OUTPUT_OVERFLOW;
    if (type == PKT_LIT)
      p_dst[pos] = prev_byte;
    else
      rep0 = dist;
    for (; len > 0; len--) {
      p_dst[pos] = prev_byte = p_dst[pos - dist];
      pos++;
    }
  }
  free(probs_literal);
  *p_dst_len = pos;
  return R_OK;
}
#define LZMA_HEADER_LEN 13
#define LZMA_DIC_MIN (1 << 12)
static int parseLzmaHeader(const uint8_t *p_src, uint8_t *p_lc, uint8_t *p_lp,
                           uint8_t *p_pb, uint32_t *p_dict_len,
                           size_t *p_uncompressed_len,
                           uint32_t *p_uncompressed_len_known) {
  uint8_t byte0 = p_src[0];
  *p_dict_len = ((uint32_t)p_src[1]) | ((uint32_t)p_src[2] << 8) |
                ((uint32_t)p_src[3] << 16) | ((uint32_t)p_src[4] << 24);
  if (*p_dict_len < LZMA_DIC_MIN)
    *p_dict_len = LZMA_DIC_MIN;
  if (p_src[5] == 0xFF && p_src[6] == 0xFF && p_src[7] == 0xFF &&
      p_src[8] == 0xFF && p_src[9] == 0xFF && p_src[10] == 0xFF &&
      p_src[11] == 0xFF && p_src[12] == 0xFF) {
    *p_uncompressed_len_known = 0;
  } else {
    uint32_t i;
    *p_uncompressed_len_known = 1;
    *p_uncompressed_len = 0;
    for (i = 0; i < 8; i++) {
      if (i < sizeof(size_t)) {
        *p_uncompressed_len |= (((size_t)p_src[5 + i]) << (i << 3));
      } else if (p_src[5 + i] > 0) {
        return R_ERR_OUTPUT_OVERFLOW;
      }
    }
  }
  *p_lc = (uint8_t)(byte0 % 9);
  byte0 /= 9;
  *p_lp = (uint8_t)(byte0 % 5);
  *p_pb = (uint8_t)(byte0 / 5);
  if (*p_lc > MAX_LC || *p_lp > MAX_LP || *p_pb > MAX_PB)
    return R_ERR_UNSUPPORTED;
  return R_OK;
}
int tinyLzmaDecompress(const uint8_t *p_src, size_t src_len, uint8_t *p_dst,
                       size_t *p_dst_len) {
  uint8_t lc, lp, pb;
  uint32_t dict_len, uncompressed_len_known;
  size_t uncompressed_len = 0;
  if (src_len < LZMA_HEADER_LEN)
    return R_ERR_INPUT_OVERFLOW;
  RET_IF_ERROR(parseLzmaHeader(p_src, &lc, &lp, &pb, &dict_len,
                               &uncompressed_len, &uncompressed_len_known))
  if (uncompressed_len_known) {
    if (uncompressed_len > *p_dst_len)
      return R_ERR_OUTPUT_OVERFLOW;
    *p_dst_len = uncompressed_len;
  } else {
  }
  RET_IF_ERROR(lzmaDecode(p_src + LZMA_HEADER_LEN, src_len - LZMA_HEADER_LEN,
                          p_dst, p_dst_len, lc, lp, pb));
  if (uncompressed_len_known && uncompressed_len != *p_dst_len)
    return R_ERR_OUTPUT_LEN_MISMATCH;
  return R_OK;
}

#include <algorithm>
#include <iostream>
#include <queue>
#include <random>
#include <vector>

#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "td/utils/misc.h"
#include "vm/boc.h"

td::BufferSlice lzma_compress(td::Slice data) {
  const std::size_t src_len = data.size();
  auto dst_len = src_len + (src_len >> 2) + 4096;
  if (dst_len > 2UL << 20)
    dst_len = 2UL << 20;
  auto output = td::BufferSlice(dst_len);
  tinyLzmaCompress(data.ubegin(), src_len,
                   reinterpret_cast<uint8_t *>(output.data()), &dst_len);
  output.truncate(dst_len);
  return output;
}
td::Result<td::BufferSlice> lzma_decompress(td::Slice data,
                                            int max_decompressed_size) {
  std::size_t dst_len = max_decompressed_size;
  auto output = td::BufferSlice(dst_len);
  tinyLzmaDecompress(data.ubegin(), data.size(),
                     reinterpret_cast<uint8_t *>(output.data()), &dst_len);
  output.truncate(dst_len);
  return output;
}

std::random_device rd;
std::mt19937 rng(rd());

const int POPULATION = 10;
const int CHILDREN = 100;
const int MUTATION = 5;
const int CROSS = 5;
const int NOT_CROSS = 1;

struct Gene {
public:
  static int number_of_cells;

  std::vector<int> perm;
  int unfitness;

  Gene(std::vector<int> perm, int unfitness)
      : perm(perm), unfitness(unfitness) {}

  Gene(bool fill = true) {
    perm.clear();
    unfitness = 1e9;

    if (!fill)
      return;

    for (int i = 0; i < number_of_cells; i++) {
      perm.push_back(i);
    }

    mutate(rng() % 10);
    if (rng() % 2)
      std::reverse(perm.begin(), perm.end());
  }

  void mutate(int cnt = 1) {
    while (cnt--) {
      int i = rng() % number_of_cells;
      int j = rng() % number_of_cells;
      std::swap(perm[i], perm[j]);
    }
  }

  void apply(std::vector<int> &ret) const {
    if (!perm.empty())
      ret = perm;
  }
};

int Gene::number_of_cells = -1;

bool operator<(const Gene &a, const Gene &b) {
  return a.unfitness < b.unfitness;
}

Gene PMX(const Gene &a, const Gene &b) {
  int n = a.perm.size();
  int l = rng() % n;
  int r = rng() % n;
  if (l > r) {
    std::swap(l, r);
  }

  std::vector<int> perm(n, -1);
  std::vector<bool> used(n);

  std::vector<int> to(n), rev_b(n);
  for (int i = 0; i < n; i++) {
    rev_b[b.perm[i]] = i;
  }

  for (int i = 0; i < n; i++) {
    to[i] = rev_b[a.perm[i]];
  }

  for (int i = l; i <= r; i++) {
    perm[i] = a.perm[i];
    used[a.perm[i]] = true;
  }

  for (int i = l; i <= r; i++) {
    if (used[b.perm[i]]) {
      continue;
    }

    int j = i;
    std::vector<int> path;
    while (perm[j] > -1) {
      path.push_back(j);
      j = to[j];
    }

    perm[j] = b.perm[i];
    used[b.perm[i]] = true;

    for (auto x : path) {
      to[x] = j;
    }
  }

  for (int i = 0; i < n; i++) {
    if (perm[i] == -1) {
      perm[i] = b.perm[i];
    }
  }

  return Gene(perm, 0);
}

Gene OX1(const Gene &a, const Gene &b) {
  int n = a.perm.size();
  int l = rng() % n;
  int r = rng() % n;
  if (l > r) {
    std::swap(l, r);
  }

  std::vector<int> perm(n, -1);
  std::vector<bool> used(n);

  for (int i = l; i <= r; i++) {
    perm[i] = a.perm[i];
    used[a.perm[i]] = true;
  }

  int j = 0;
  for (int i = 0; i < n; i++) {
    if (used[b.perm[i]]) {
      continue;
    }

    if (j == l) {
      j = r + 1;
    }

    perm[j] = b.perm[i];
    j++;
  }

  return Gene(perm, 0);
}

Gene merge(const Gene &a, const Gene &b) {
  if (rng() % (CROSS + NOT_CROSS) < NOT_CROSS)
    return a;
  if (rng() % 2)
    return PMX(a, b);
  return OX1(a, b);
}

struct CellSerializationInfo {
  bool special;
  vm::Cell::LevelMask level_mask;

  bool with_hashes;
  size_t hashes_offset;
  size_t depth_offset;

  size_t data_offset;
  size_t data_len;
  bool data_with_bits;

  size_t refs_offset;
  int refs_cnt;

  size_t end_offset;

  td::Status init(td::Slice data, int ref_byte_size) {
    if (data.size() < 2) {
      return td::Status::Error();
    }
    TRY_STATUS(init(data.ubegin()[0], data.ubegin()[1], ref_byte_size));
    if (data.size() < end_offset) {
      return td::Status::Error();
    }
    return td::Status::OK();
  }

  td::Status init(td::uint8 d1, td::uint8 d2, int ref_byte_size) {
    refs_cnt = d1 & 7;
    level_mask = vm::Cell::LevelMask(d1 >> 5);
    special = (d1 & 8) != 0;
    with_hashes = (d1 & 16) != 0;

    if (refs_cnt > 4) {
      if (refs_cnt != 7 || !with_hashes) {
        return td::Status::Error();
      }
      refs_cnt = 0;
      return td::Status::Error();
    }

    hashes_offset = 2;
    auto n = level_mask.get_hashes_count();
    depth_offset = hashes_offset + (with_hashes ? n * vm::Cell::hash_bytes : 0);
    data_offset = depth_offset + (with_hashes ? n * vm::Cell::depth_bytes : 0);
    data_len = (d2 >> 1) + (d2 & 1);
    data_with_bits = (d2 & 1) != 0;
    refs_offset = data_offset + data_len;
    end_offset = refs_offset + refs_cnt * ref_byte_size;

    return td::Status::OK();
  }
  td::Result<int> get_bits(td::Slice cell) const {
    if (data_with_bits) {
      DCHECK(data_len != 0);
      int last = cell[data_offset + data_len - 1];
      if (!(last & 0x7f)) {
        return td::Status::Error();
      }
      return td::narrow_cast<int>((data_len - 1) * 8 + 7 -
                                  td::count_trailing_zeroes_non_zero32(last));
    } else {
      return td::narrow_cast<int>(data_len * 8);
    }
  }

  td::Result<td::Ref<vm::DataCell>>
  create_data_cell(td::Slice cell_slice,
                   td::Span<td::Ref<vm::Cell>> refs) const {
    vm::CellBuilder cb;
    TRY_RESULT(bits, get_bits(cell_slice));
    cb.store_bits(cell_slice.ubegin() + data_offset, bits);
    DCHECK(refs_cnt == (td::int64)refs.size());
    for (int k = 0; k < refs_cnt; k++) {
      cb.store_ref(std::move(refs[k]));
    }
    TRY_RESULT(res, cb.finalize_novm_nothrow(special));
    CHECK(!res.is_null());
    if (res->is_special() != special) {
      return td::Status::Error();
    }
    if (res->get_level_mask() != level_mask) {
      return td::Status::Error();
    }
    if (with_hashes) {
      auto hash_n = level_mask.get_hashes_count();
      if (res->get_hash().as_slice() !=
          cell_slice.substr(hashes_offset + vm::Cell::hash_bytes * (hash_n - 1),
                            vm::Cell::hash_bytes)) {
        return td::Status::Error();
      }
      if (res->get_depth() !=
          vm::DataCell::load_depth(
              cell_slice
                  .substr(depth_offset + vm::Cell::depth_bytes * (hash_n - 1),
                          vm::Cell::depth_bytes)
                  .ubegin())) {
        return td::Status::Error();
      }

      bool check_all_hashes = true;
      for (unsigned level_i = 0, hash_i = 0, level = level_mask.get_level();
           check_all_hashes && level_i < level; level_i++) {
        if (!level_mask.is_significant(level_i)) {
          continue;
        }
        if (cell_slice.substr(hashes_offset + vm::Cell::hash_bytes * hash_i,
                              vm::Cell::hash_bytes) !=
            res->get_hash(level_i).as_slice()) {
          return td::Status::Error();
        }
        if (res->get_depth(level_i) !=
            vm::DataCell::load_depth(
                cell_slice
                    .substr(depth_offset + vm::Cell::depth_bytes * hash_i,
                            vm::Cell::depth_bytes)
                    .ubegin())) {
          return td::Status::Error();
        }
        hash_i++;
      }
    }
    return res;
  }
};

class MyBagOfCells {
public:
  enum { hash_bytes = vm::Cell::hash_bytes, default_max_roots = 16384 };
  enum Mode {
    WithIndex = 1,
    WithCRC32C = 2,
    WithTopHash = 4,
    WithIntHashes = 8,
    WithCacheBits = 16,
    max = 31
  };
  enum { max_cell_whs = 64 };
  using Hash = vm::Cell::Hash;
  struct Info {
    enum : td::uint32 {
      boc_idx = 0x68ff65f3,
      boc_idx_crc32c = 0xacc3a728,
      boc_generic = 0xb5ee9c72
    };

    unsigned magic;
    int root_count;
    int cell_count;
    int absent_count;
    int ref_byte_size;
    int offset_byte_size;
    bool valid;
    bool has_index;
    bool has_roots{false};
    bool has_crc32c;
    bool has_cache_bits;
    unsigned long long roots_offset, index_offset, data_offset, data_size,
        total_size;
    Info() : magic(0), valid(false) {}
    void invalidate() { valid = false; }

    unsigned long long read_ref(const unsigned char *ptr) {
      return read_int(ptr, ref_byte_size);
    }
    unsigned long long read_offset(const unsigned char *ptr) {
      return read_int(ptr, offset_byte_size);
    }

    unsigned long long read_int(const unsigned char *ptr, unsigned bytes) {
      unsigned long long res = 0;
      while (bytes > 0) {
        res = (res << 8) + *ptr++;
        --bytes;
      }
      return res;
    }

    void write_int(unsigned char *ptr, unsigned long long value, int bytes) {
      ptr += bytes;
      while (bytes) {
        *--ptr = value & 0xff;
        value >>= 8;
        --bytes;
      }
      DCHECK(!bytes);
    }

    long long parse_serialized_header(const td::Slice &slice) {
      invalidate();
      int sz = static_cast<int>(
          std::min(slice.size(), static_cast<std::size_t>(0xffff)));
      if (sz < 4) {
        return -10;
      }
      const unsigned char *ptr = slice.ubegin();
      magic = (unsigned)read_int(ptr, 4);
      has_crc32c = false;
      has_index = false;
      has_cache_bits = false;
      ref_byte_size = 0;
      offset_byte_size = 0;
      root_count = cell_count = absent_count = -1;
      index_offset = data_offset = data_size = total_size = 0;
      if (magic != boc_generic && magic != boc_idx && magic != boc_idx_crc32c) {
        magic = 0;
        return 0;
      }
      if (sz < 5) {
        return -10;
      }
      td::uint8 byte = ptr[4];
      if (magic == boc_generic) {
        has_index = (byte >> 7) % 2 == 1;
        has_crc32c = (byte >> 6) % 2 == 1;
        has_cache_bits = (byte >> 5) % 2 == 1;
      } else {
        has_index = true;
        has_crc32c = magic == boc_idx_crc32c;
      }
      if (has_cache_bits && !has_index) {
        return 0;
      }
      ref_byte_size = byte & 7;
      if (ref_byte_size > 4 || ref_byte_size < 1) {
        return 0;
      }
      if (sz < 6) {
        return -7 - 3 * ref_byte_size;
      }
      offset_byte_size = ptr[5];
      if (offset_byte_size > 8 || offset_byte_size < 1) {
        return 0;
      }
      roots_offset = 6 + 3 * ref_byte_size + offset_byte_size;
      ptr += 6;
      sz -= 6;
      if (sz < ref_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      cell_count = (int)read_ref(ptr);
      if (cell_count <= 0) {
        cell_count = -1;
        return 0;
      }
      if (sz < 2 * ref_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      root_count = (int)read_ref(ptr + ref_byte_size);
      if (root_count <= 0) {
        root_count = -1;
        return 0;
      }
      index_offset = roots_offset;
      if (magic == boc_generic) {
        index_offset += (long long)root_count * ref_byte_size;
        has_roots = true;
      } else {
        if (root_count != 1) {
          return 0;
        }
      }
      data_offset = index_offset;
      if (has_index) {
        data_offset += (long long)cell_count * offset_byte_size;
      }
      if (sz < 3 * ref_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      absent_count = (int)read_ref(ptr + 2 * ref_byte_size);
      if (absent_count < 0 || absent_count > cell_count) {
        return 0;
      }
      if (sz < 3 * ref_byte_size + offset_byte_size) {
        return -static_cast<int>(roots_offset);
      }
      data_size = read_offset(ptr + 3 * ref_byte_size);
      if (data_size > ((unsigned long long)cell_count << 10)) {
        return 0;
      }
      if (data_size > (1ull << 40)) {
        return 0;
      }
      if (data_size < cell_count * (2ull + ref_byte_size) - ref_byte_size) {
        return 0;
      }
      valid = true;
      total_size = data_offset + data_size + (has_crc32c ? 4 : 0);
      return total_size;
    }
  };

public:
  int cell_count{0}, root_count{0}, dangle_count{0}, int_refs{0};
  int int_hashes{0}, top_hashes{0};
  int max_depth{1024};
  Info info;
  unsigned long long data_bytes{0};
  td::HashMap<Hash, int> cells;
  struct CellInfo {
    td::Ref<vm::DataCell> dc_ref;
    std::array<int, 4> ref_idx;
    unsigned char ref_num;
    unsigned char wt;
    unsigned char hcnt;
    int new_idx;
    bool should_cache{false};
    bool is_root_cell{false};
    CellInfo() : ref_num(0) {}
    CellInfo(td::Ref<vm::DataCell> _dc) : dc_ref(std::move(_dc)), ref_num(0) {}
    CellInfo(td::Ref<vm::DataCell> _dc, int _refs,
             const std::array<int, 4> &_ref_list)
        : dc_ref(std::move(_dc)), ref_idx(_ref_list),
          ref_num(static_cast<unsigned char>(_refs)) {}
    bool is_special() const { return !wt; }
  };
  std::vector<CellInfo> cell_list_;
  struct RootInfo {
    RootInfo() = default;
    RootInfo(td::Ref<vm::Cell> cell, int idx)
        : cell(std::move(cell)), idx(idx) {}
    td::Ref<vm::Cell> cell;
    int idx{-1};
  };
  std::vector<CellInfo> cell_list_tmp;
  std::vector<RootInfo> roots;
  std::vector<unsigned char> serialized;
  const unsigned char *index_ptr{nullptr};
  const unsigned char *data_ptr{nullptr};
  std::vector<unsigned long long> custom_index;

public:
  MyBagOfCells() = default;
  int get_root_count() const { return root_count; }

  void clear() {
    cells_clear();
    roots.clear();
    root_count = 0;
    serialized.clear();
  }

  bool get_cache_entry(int index) {
    if (!info.has_cache_bits) {
      return true;
    }
    if (!info.has_index) {
      return true;
    }
    auto raw = get_idx_entry_raw(index);
    return raw % 2 == 1;
  }

  unsigned long long get_idx_entry_raw(int index) {
    if (index < 0) {
      return 0;
    }
    if (!info.has_index) {
      return custom_index.at(index);
    } else if (index < info.cell_count && index_ptr) {
      return info.read_offset(index_ptr + (long)index * info.offset_byte_size);
    } else {
      return 0;
    }
  }

  unsigned long long get_idx_entry(int index) {
    auto raw = get_idx_entry_raw(index);
    if (info.has_cache_bits) {
      raw /= 2;
    }
    return raw;
  }

  td::Result<td::Slice> get_cell_slice(int idx, td::Slice data) {
    unsigned long long offs = get_idx_entry(idx - 1);
    unsigned long long offs_end = get_idx_entry(idx);
    if (offs > offs_end || offs_end > data.size()) {
      return td::Status::Error();
    }
    return data.substr(offs, td::narrow_cast<size_t>(offs_end - offs));
  }

  td::Result<std::vector<int>> get_topol_order(td::Slice cells_slice,
                                               int cell_count) {
    std::vector<std::vector<int>> rev_graph(cell_count);
    std::vector<int> in_degree(cell_count, 0);

    for (int idx = 0; idx < cell_count; idx++) {
      TRY_RESULT(cell_slice, get_cell_slice(idx, cells_slice));

      CellSerializationInfo cell_info;
      TRY_STATUS(cell_info.init(cell_slice, info.ref_byte_size));
      if (cell_info.end_offset != cell_slice.size()) {
        return td::Status::Error();
      }

      for (int k = 0; k < cell_info.refs_cnt; k++) {
        int ref_idx =
            (int)info.read_ref(cell_slice.ubegin() + cell_info.refs_offset +
                               k * info.ref_byte_size);

        rev_graph[ref_idx].push_back(idx);
        in_degree[idx]++;
      }
    }

    std::vector<int> topol;
    std::queue<int> q;
    for (int idx = 0; idx < cell_count; idx++) {
      if (!in_degree[idx]) {
        q.push(idx);
      }
    }

    while (!q.empty()) {
      int idx = q.front();
      q.pop();
      topol.push_back(idx);

      for (int ref_idx : rev_graph[idx]) {
        in_degree[ref_idx]--;
        if (!in_degree[ref_idx]) {
          q.push(ref_idx);
        }
      }
    }

    return topol;
  }

  td::Result<td::Ref<vm::DataCell>>
  deserialize_cell(std::vector<int> idx_map, int idx, td::Slice cells_slice,
                   td::Span<td::Ref<vm::DataCell>> cells_span,
                   std::vector<td::uint8> *cell_should_cache) {
    TRY_RESULT(cell_slice, get_cell_slice(idx, cells_slice));
    std::array<td::Ref<vm::Cell>, 4> refs_buf;

    CellSerializationInfo cell_info;
    TRY_STATUS(cell_info.init(cell_slice, info.ref_byte_size));
    if (cell_info.end_offset != cell_slice.size()) {
      return td::Status::Error();
    }

    auto refs = td::MutableSpan<td::Ref<vm::Cell>>(refs_buf).substr(
        0, cell_info.refs_cnt);
    for (int k = 0; k < cell_info.refs_cnt; k++) {
      int ref_idx = (int)info.read_ref(
          cell_slice.ubegin() + cell_info.refs_offset + k * info.ref_byte_size);
      if (idx_map[ref_idx] == -1) {
        return td::Status::Error();
      }
      if (ref_idx >= cell_count) {
        return td::Status::Error();
      }
      refs[k] = cells_span[idx_map[ref_idx]];
      if (cell_should_cache) {
        auto &cnt = (*cell_should_cache)[ref_idx];
        if (cnt < 2) {
          cnt++;
        }
      }
    }

    return cell_info.create_data_cell(cell_slice, refs);
  }

  td::Result<long long> deserialize(const td::Slice &data, int max_roots) {
    clear();
    long long size_est = info.parse_serialized_header(data);
    if (size_est == 0) {
      return td::Status::Error();
    }
    if (size_est < 0) {
      return size_est;
    }

    if (size_est > (long long)data.size()) {
      return -size_est;
    }
    if (info.root_count > max_roots) {
      return td::Status::Error();
    }
    if (info.has_crc32c) {
      unsigned crc_computed =
          td::crc32c(td::Slice{data.ubegin(), data.uend() - 4});
      unsigned crc_stored = td::as<unsigned>(data.uend() - 4);
      if (crc_computed != crc_stored) {
        return td::Status::Error();
      }
    }

    cell_count = info.cell_count;
    std::vector<td::uint8> cell_should_cache;
    if (info.has_cache_bits) {
      cell_should_cache.resize(cell_count, 0);
    }
    roots.clear();
    roots.resize(info.root_count);
    auto *roots_ptr = data.substr(info.roots_offset).ubegin();
    for (int i = 0; i < info.root_count; i++) {
      int idx = 0;
      if (info.has_roots) {
        idx = (int)info.read_ref(roots_ptr + i * info.ref_byte_size);
      }
      if (idx < 0 || idx >= info.cell_count) {
        return td::Status::Error();
      }
      roots[i].idx = idx;
      if (info.has_cache_bits) {
        auto &cnt = cell_should_cache[idx];
        if (cnt < 2) {
          cnt++;
        }
      }
    }
    if (info.has_index) {
      index_ptr = data.substr(info.index_offset).ubegin();
    } else {
      index_ptr = nullptr;
      unsigned long long cur = 0;
      custom_index.reserve(info.cell_count);

      auto cells_slice = data.substr(info.data_offset, info.data_size);

      for (int i = 0; i < info.cell_count; i++) {
        CellSerializationInfo cell_info;
        auto status = cell_info.init(cells_slice, info.ref_byte_size);
        if (status.is_error()) {
          return td::Status::Error();
        }
        cells_slice = cells_slice.substr(cell_info.end_offset);
        cur += cell_info.end_offset;
        custom_index.push_back(cur);
      }
      if (!cells_slice.empty()) {
        return td::Status::Error();
      }
    }
    auto cells_slice = data.substr(info.data_offset, info.data_size);
    std::vector<td::Ref<vm::DataCell>> cell_list;
    cell_list.reserve(cell_count);
    std::array<td::Ref<vm::Cell>, 4> refs_buf;

    auto topol_order = get_topol_order(cells_slice, cell_count).move_as_ok();
    std::vector<int> idx_map(cell_count, -1);

    for (auto &&idx : topol_order) {
      auto r_cell =
          deserialize_cell(idx_map, idx, cells_slice, cell_list,
                           info.has_cache_bits ? &cell_should_cache : nullptr);
      if (r_cell.is_error()) {
        return td::Status::Error();
      }
      idx_map[idx] = cell_list.size();
      cell_list.push_back(r_cell.move_as_ok());
      DCHECK(cell_list.back().not_null());
    }

    if (info.has_cache_bits) {
      for (int idx = 0; idx < cell_count; idx++) {
        auto should_cache = cell_should_cache[idx] > 1;
        auto stored_should_cache = get_cache_entry(idx);
        if (should_cache != stored_should_cache) {
          return td::Status::Error();
        }
      }
    }
    custom_index.clear();
    index_ptr = nullptr;
    root_count = info.root_count;
    dangle_count = info.absent_count;
    for (auto &root_info : roots) {
      root_info.cell = cell_list[idx_map[root_info.idx]];
    }
    cell_list.clear();
    return size_est;
  }

  td::Ref<vm::Cell> get_root_cell(int idx = 0) const {
    return (idx >= 0 && idx < root_count) ? roots.at(idx).cell
                                          : td::Ref<vm::Cell>{};
  }
  void permute(const Gene &gene) {
    std::vector<int> perm(cell_count);
    for (int i = 0; i < cell_count; i++) {
      perm[i] = i;
    }
    gene.apply(perm);
    for (int i = 0; i < cell_count; i++) {
      cell_list_[i].new_idx = perm[i];
    }

    for (int i = 0; i < cell_count; i++) {
      for (int j = 0; j < cell_list_[i].ref_num; j++) {
        cell_list_[i].ref_idx[j] = perm[cell_list_[i].ref_idx[j]];
      }
    }

    for (int i = 0; i < root_count; i++) {
      roots[i].idx = perm[roots[i].idx];
    }
    std::sort(cell_list_.begin(), cell_list_.end(),
              [](const CellInfo &a, const CellInfo &b) {
                return a.new_idx < b.new_idx;
              });
    for (int i = 0; i < cell_count; i++) {
      cells[cell_list_[i].dc_ref->get_hash()] = i;
    }
  }

private:
  int rv_idx;
  void cells_clear() {
    cell_count = 0;
    int_refs = 0;
    data_bytes = 0;
    cells.clear();
    cell_list_.clear();
  }
};

td::Result<td::BufferSlice>
my_std_boc_serialize(const Gene &gene, td::Ref<vm::Cell> root, int mode = 0) {
  if (root.is_null()) {
    return td::Status::Error();
  }
  vm::BagOfCells boc;
  boc.add_root(std::move(root));
  auto res = boc.import_cells();

  auto myBoc = reinterpret_cast<MyBagOfCells *>(&boc);
  myBoc->permute(gene);
  Gene::number_of_cells = myBoc->cell_count;

  if (res.is_error()) {
    return res.move_as_error();
  }
  return boc.serialize_to_slice(mode);
}

td::Result<td::Ref<vm::Cell>>
my_std_boc_deserialize(td::Slice data, bool can_be_empty = false,
                       bool allow_nonzero_level = false) {
  if (data.empty() && can_be_empty) {
    return td::Ref<vm::Cell>();
  }
  vm::BagOfCells boc;
  auto myBoc = reinterpret_cast<MyBagOfCells *>(&boc);
  auto res = myBoc->deserialize(data, 1);
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 1) {
    return td::Status::Error();
  }
  auto root = boc.get_root_cell();
  if (root.is_null()) {
    return td::Status::Error();
  }
  if (!allow_nonzero_level && root->get_level() != 0) {
    return td::Status::Error();
  }
  return std::move(root);
}

td::BufferSlice compress(td::Slice data) {
  const auto start_time = std::chrono::steady_clock::now();

  const auto is_timeout = [&start_time](int timeout = 1900) {
    auto MAX_DURATION = std::chrono::milliseconds(timeout);

    auto now = std::chrono::steady_clock::now();
    return (now - start_time) >= MAX_DURATION;
  };

  td::Ref<vm::Cell> root = vm::std_boc_deserialize(data).move_as_ok();

  td::BufferSlice best =
      lzma_compress(my_std_boc_serialize(Gene(false), root, 2).move_as_ok());
  int normal_len = best.length();
  int attempts = 0;

  auto evalGene = [&](Gene &gene) {
    if (is_timeout(1800)) {
      gene.unfitness = -1;
      return;
    }
    auto ser = my_std_boc_serialize(gene, root, 2).move_as_ok();
    auto compressed = lzma_compress(ser);
    gene.unfitness = compressed.length();

    if (compressed.length() < best.length()) {
      best = std::move(compressed);
    }
  };

  std::vector<Gene> population;
  for (int i = 0; i < POPULATION; i++) {
    population.push_back(Gene());
    evalGene(population.back());

    if (is_timeout()) {
      return best;
    }
  }
  std::sort(population.begin(), population.end());

  while (true) {
    attempts++;

    std::vector<Gene> childs, new_population;
    std::vector<long long> partial_sum_unfitness;
    long long tot_unfitness = 0;
    for (auto &gene : population) {
      tot_unfitness += gene.unfitness;
      partial_sum_unfitness.push_back(tot_unfitness);
    }

    auto get_random_by_unfittness = [&]() -> Gene & {
      long long rnd = rng() % tot_unfitness;
      int ind = std::lower_bound(partial_sum_unfitness.begin(),
                                 partial_sum_unfitness.end(), rnd) -
                partial_sum_unfitness.begin();
      return population[ind];
    };

    for (int i = 0; i < CHILDREN; i++) {
      if (is_timeout()) {
        break;
      }
      auto child =
          merge(get_random_by_unfittness(), get_random_by_unfittness());
      child.mutate(rng() % MUTATION);
      evalGene(child);
      if (child.unfitness == -1)
        return best;
      if (is_timeout()) {
        break;
      }
      childs.push_back(child);
    }
    if (is_timeout()) {
      break;
    }

    std::sort(childs.begin(), childs.end());
    childs.resize(POPULATION);

    population = std::move(childs);

    if (is_timeout()) {
      break;
    }
  }
  return best;
}

td::BufferSlice decompress(td::Slice data) {
  td::BufferSlice serialized = lzma_decompress(data, 2 << 20).move_as_ok();
  auto root = my_std_boc_deserialize(serialized).move_as_ok();
  return vm::std_boc_serialize(root, 31).move_as_ok();
}

int main() {
  std::string mode;
  std::cin >> mode;
  CHECK(mode == "compress" || mode == "decompress");

  std::string base64_data;
  std::cin >> base64_data;
  CHECK(!base64_data.empty());

  td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());

  if (mode == "compress") {
    data = compress(data);
  } else {
    data = decompress(data);
  }

  std::cout << td::base64_encode(data) << std::endl;
}
