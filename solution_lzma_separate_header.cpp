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
#include <vector>

#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "td/utils/lz4.h"
#include "td/utils/misc.h"
#include "vm/boc-writers.h"
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

class MyDataCell : public vm::Cell {
public:
  // NB: cells created with use_arena=true are never freed
  static thread_local bool use_arena;

  MyDataCell(const MyDataCell &other) = delete;
  ~MyDataCell() override;

  static void store_depth(td::uint8 *dest, td::uint16 depth) {
    td::bitstring::bits_store_long(dest, depth, depth_bits);
  }
  static td::uint16 load_depth(const td::uint8 *src) {
    return td::bitstring::bits_load_ulong(src, depth_bits) & 0xffff;
  }

public:
  struct Info {
    unsigned bits_;

    // d1
    unsigned char refs_count_ : 3;
    bool is_special_ : 1;
    unsigned char level_mask_ : 3;

    unsigned char hash_count_ : 3;

    unsigned char virtualization_ : 3;

    unsigned char d1() const { return d1(LevelMask{level_mask_}); }
    unsigned char d1(LevelMask level_mask) const {
      // d1 = refs_count + 8 * is_special + 32 * level
      //      + 16 * with_hashes - for seriazlization
      // d1 = 7 + 16 + 32 * l - for absent cells
      return static_cast<unsigned char>(refs_count_ + 8 * is_special_ +
                                        32 * level_mask.get_mask());
    }
    unsigned char d2() const {
      auto res = static_cast<unsigned char>((bits_ / 8) * 2);
      if ((bits_ & 7) != 0) {
        return static_cast<unsigned char>(res + 1);
      }
      return res;
    }
    size_t get_hashes_offset() const { return 0; }
    size_t get_refs_offset() const {
      return get_hashes_offset() + hash_bytes * hash_count_;
    }
    size_t get_depth_offset() const {
      return get_refs_offset() + refs_count_ * sizeof(Cell *);
    }
    size_t get_data_offset() const {
      return get_depth_offset() + sizeof(td::uint16) * hash_count_;
    }
    size_t get_storage_size() const {
      return get_data_offset() + (bits_ + 7) / 8;
    }

    const Hash *get_hashes(const char *storage) const {
      return reinterpret_cast<const Hash *>(storage + get_hashes_offset());
    }

    Hash *get_hashes(char *storage) const {
      return reinterpret_cast<Hash *>(storage + get_hashes_offset());
    }

    const td::uint16 *get_depth(const char *storage) const {
      return reinterpret_cast<const td::uint16 *>(storage + get_depth_offset());
    }

    td::uint16 *get_depth(char *storage) const {
      return reinterpret_cast<td::uint16 *>(storage + get_depth_offset());
    }

    const unsigned char *get_data(const char *storage) const {
      return reinterpret_cast<const unsigned char *>(storage +
                                                     get_data_offset());
    }
    unsigned char *get_data(char *storage) const {
      return reinterpret_cast<unsigned char *>(storage + get_data_offset());
    }

    Cell *const *get_refs(const char *storage) const {
      return reinterpret_cast<Cell *const *>(storage + get_refs_offset());
    }
    Cell **get_refs(char *storage) const {
      return reinterpret_cast<Cell **>(storage + get_refs_offset());
    }
  };

  Info info_;
  virtual char *get_storage() = 0;
  virtual const char *get_storage() const = 0;
  // TODO: we may also save three different pointers

  void destroy_storage(char *storage);

  explicit MyDataCell(Info info);

public:
  unsigned get_refs_cnt() const { return info_.refs_count_; }
  unsigned get_bits() const { return info_.bits_; }
  unsigned size_refs() const { return info_.refs_count_; }
  unsigned size() const { return info_.bits_; }
  const unsigned char *get_data() const {
    return info_.get_data(get_storage());
  }

  Cell *get_ref_raw_ptr(unsigned idx) const {
    DCHECK(idx < get_refs_cnt());
    return info_.get_refs(get_storage())[idx];
  }

  td::uint32 get_virtualization() const override {
    return info_.virtualization_;
  }
  bool is_loaded() const override { return true; }
  LevelMask get_level_mask() const override {
    return LevelMask{info_.level_mask_};
  }

  bool is_special() const { return info_.is_special_; }
  SpecialType special_type() const;
  int get_serialized_size(bool with_hashes = false) const {
    return ((get_bits() + 23) >> 3) +
           (with_hashes ? get_level_mask().get_hashes_count() *
                              (hash_bytes + depth_bytes)
                        : 0);
  }
  size_t get_storage_size() const { return info_.get_storage_size(); }

  std::pair<unsigned char, unsigned char>
  get_first_two_meta(bool with_hashes = false) const {
    return {static_cast<unsigned char>(info_.d1() | (with_hashes * 16)),
            info_.d2()};
  }

  int serialize(unsigned char *buff, int buff_size, bool with_hashes = false,
                bool with_meta = false) const {
    int len = get_serialized_size(with_hashes);
    if (len > buff_size) {
      return 0;
    }

    if (with_meta) {
      buff[0] = static_cast<unsigned char>(info_.d1() | (with_hashes * 16));
      buff[1] = info_.d2();
    }

    int hs = 0;
    if (with_hashes) {
      hs = (get_level_mask().get_hashes_count()) * (hash_bytes + depth_bytes);
      assert(len >= 2 + hs);
      std::memset(buff + 2, 0, hs);
      auto dest = td::MutableSlice(buff + 2, hs);
      auto level = get_level();
      // TODO: optimize for prunned brandh
      for (unsigned i = 0; i <= level; i++) {
        if (!get_level_mask().is_significant(i)) {
          continue;
        }
        dest.copy_from(get_hash(i).as_slice());
        dest.remove_prefix(hash_bytes);
      }
      for (unsigned i = 0; i <= level; i++) {
        if (!get_level_mask().is_significant(i)) {
          continue;
        }
        store_depth(dest.ubegin(), get_depth(i));
        dest.remove_prefix(depth_bytes);
      }
      // buff[2] = 0;  // for testing hash verification in deserialization
      buff += hs;
      len -= hs;
    }
    std::memcpy(buff + (with_meta ? 2 : 0), get_data(), len - 2);
    return len - (with_meta ? 0 : 2) + hs;
  }

  std::string serialize() const;
  std::string to_hex() const;

  template <class StorerT> void store(StorerT &storer) const {
    storer.template store_binary<td::uint8>(info_.d1());
    storer.template store_binary<td::uint8>(info_.d2());
    storer.store_slice(td::Slice(get_data(), (get_bits() + 7) / 8));
  }

protected:
  static constexpr auto max_storage_size =
      max_refs * sizeof(void *) + (max_level + 1) * hash_bytes + max_bytes;

private:
};

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
      return td::Status::Error(PSLICE() << "Not enough bytes "
                                        << td::tag("got", data.size())
                                        << td::tag("expected", "at least 2"));
    }
    TRY_STATUS(init(data.ubegin()[0], data.ubegin()[1], ref_byte_size));
    if (data.size() < end_offset) {
      return td::Status::Error(PSLICE() << "Not enough bytes "
                                        << td::tag("got", data.size())
                                        << td::tag("expected", end_offset));
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
        return td::Status::Error("Invalid first byte");
      }
      refs_cnt = 0;
      // ...
      // do not deserialize absent cells!
      return td::Status::Error("TODO: absent cells");
    }

    hashes_offset = 2;
    auto n = level_mask.get_hashes_count();
    depth_offset = hashes_offset + (with_hashes ? n * vm::Cell::hash_bytes : 0);
    data_offset = -1; // FIXME depth_offset + (with_hashes ? n *
                      // vm::Cell::depth_bytes : 0);
    data_len = (d2 >> 1) + (d2 & 1);
    data_with_bits = (d2 & 1) != 0;
    // refs_offset = data_offset + data_len;
    refs_offset = depth_offset + (with_hashes ? n * vm::Cell::depth_bytes : 0);
    end_offset = refs_offset + refs_cnt * ref_byte_size;

    return td::Status::OK();
  }
  td::Result<int> get_bits(td::Slice data) const {
    if (data_with_bits) {
      DCHECK(data_len != 0);
      int last = data[data_len - 1];
      if (!(last & 0x7f)) {
        return td::Status::Error("overlong encoding");
      }
      return td::narrow_cast<int>((data_len - 1) * 8 + 7 -
                                  td::count_trailing_zeroes_non_zero32(last));
    } else {
      return td::narrow_cast<int>(data_len * 8);
    }
  }

  td::Result<td::Ref<vm::DataCell>>
  create_data_cell(td::Slice cell_slice, td::Slice data_slice,
                   unsigned long long cell_data_offset,
                   td::Span<td::Ref<vm::Cell>> refs) const {
    vm::CellBuilder cb;
    TRY_RESULT(bits, get_bits(data_slice.substr(cell_data_offset)));
    cb.store_bits(data_slice.ubegin() + cell_data_offset, bits);

    DCHECK(refs_cnt == (td::int64)refs.size());
    for (int k = 0; k < refs_cnt; k++) {
      cb.store_ref(std::move(refs[k]));
    }
    TRY_RESULT(res, cb.finalize_novm_nothrow(special));
    CHECK(!res.is_null());
    if (res->is_special() != special) {
      return td::Status::Error("is_special mismatch");
    }
    if (res->get_level_mask() != level_mask) {
      return td::Status::Error("level mask mismatch");
    }
    // return res;
    if (with_hashes) {
      assert(false);
      auto hash_n = level_mask.get_hashes_count();
      if (res->get_hash().as_slice() !=
          cell_slice.substr(hashes_offset + vm::Cell::hash_bytes * (hash_n - 1),
                            vm::Cell::hash_bytes)) {
        return td::Status::Error("representation hash mismatch");
      }
      if (res->get_depth() !=
          vm::DataCell::load_depth(
              cell_slice
                  .substr(depth_offset + vm::Cell::depth_bytes * (hash_n - 1),
                          vm::Cell::depth_bytes)
                  .ubegin())) {
        return td::Status::Error("depth mismatch");
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
          // hash mismatch
          return td::Status::Error("lower hash mismatch");
        }
        if (res->get_depth(level_i) !=
            vm::DataCell::load_depth(
                cell_slice
                    .substr(depth_offset + vm::Cell::depth_bytes * hash_i,
                            vm::Cell::depth_bytes)
                    .ubegin())) {
          return td::Status::Error("lower depth mismatch");
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
        return -10; // want at least 10 bytes
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
        return 0; // bag of cells with more than 1TiB data is unlikely
      }
      if (data_size < cell_count * (2ull + ref_byte_size) - ref_byte_size) {
        return 0; // invalid header, too many cells for this amount of data
                  // bytes
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
      assert(false);
      return info.read_offset(index_ptr + (long)index * info.offset_byte_size);
    } else {
      assert(false);
      // throw ?
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
      return td::Status::Error(PSLICE() << "invalid index entry [" << offs
                                        << "; " << offs_end << "], "
                                        << td::tag("data.size()", data.size()));
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
        return td::Status::Error("unused space in cell serialization");
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

  td::Result<int> get_cell_meta_size(int idx, td::Slice cells_slice) {}

  td::Result<td::Ref<vm::DataCell>>
  deserialize_cell(std::vector<int> idx_map, int idx, td::Slice cells_slice,
                   td::Slice data_slice, unsigned long long cell_data_offset,
                   td::Span<td::Ref<vm::DataCell>> cells_span,
                   std::vector<td::uint8> *cell_should_cache) {
    TRY_RESULT(cell_slice, get_cell_slice(idx, cells_slice));
    std::array<td::Ref<vm::Cell>, 4> refs_buf;

    CellSerializationInfo cell_info;
    TRY_STATUS(cell_info.init(cell_slice, info.ref_byte_size));
    if (cell_info.end_offset != cell_slice.size()) {
      return td::Status::Error("unused space in cell serialization");
    }

    auto refs = td::MutableSpan<td::Ref<vm::Cell>>(refs_buf).substr(
        0, cell_info.refs_cnt);
    for (int k = 0; k < cell_info.refs_cnt; k++) {
      int ref_idx = (int)info.read_ref(
          cell_slice.ubegin() + cell_info.refs_offset + k * info.ref_byte_size);
      if (idx_map[ref_idx] == -1) {
        return td::Status::Error(
            PSLICE() << "bag-of-cells error: reference #" << k << " of cell #"
                     << idx << " is to cell #" << ref_idx
                     << " which does not appear earlier in topological order");
      }
      if (ref_idx >= cell_count) {
        return td::Status::Error(
            PSLICE() << "bag-of-cells error: reference #" << k << " of cell #"
                     << idx << " is to non-existent cell #" << ref_idx
                     << ", only " << cell_count << " cells are defined");
      }
      refs[k] = cells_span[idx_map[ref_idx]];
      if (cell_should_cache) {
        auto &cnt = (*cell_should_cache)[ref_idx];
        if (cnt < 2) {
          cnt++;
        }
      }
    }

    return cell_info.create_data_cell(cell_slice, data_slice, cell_data_offset,
                                      refs);
  }

  td::Result<long long> deserialize(const td::Slice &data, int max_roots) {
    clear();
    long long size_est = info.parse_serialized_header(data);
    // LOG(INFO) << "estimated size " << size_est << ", true size " <<
    // data.size();
    if (size_est == 0) {
      return td::Status::Error(
          PSLICE() << "cannot deserialize bag-of-cells: invalid header, error "
                   << size_est);
    }
    if (size_est < 0) {
      // LOG(ERROR) << "cannot deserialize bag-of-cells: not enough bytes (" <<
      // data.size() << " present, " << -size_est
      //<< " required)";
      return size_est;
    }

    if (size_est > (long long)data.size()) {
      // LOG(ERROR) << "cannot deserialize bag-of-cells: not enough bytes (" <<
      // data.size() << " present, " << size_est
      //<< " required)";
      return -size_est;
    }
    // LOG(INFO) << "estimated size " << size_est << ", true size " <<
    // data.size();
    if (info.root_count > max_roots) {
      return td::Status::Error(
          "Bag-of-cells has more root cells than expected");
    }
    if (info.has_crc32c) {
      unsigned crc_computed =
          td::crc32c(td::Slice{data.ubegin(), data.uend() - 4});
      unsigned crc_stored = td::as<unsigned>(data.uend() - 4);
      if (crc_computed != crc_stored) {
        return td::Status::Error(
            PSLICE() << "bag-of-cells CRC32C mismatch: expected "
                     << td::format::as_hex(crc_computed) << ", found "
                     << td::format::as_hex(crc_stored));
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
        return td::Status::Error(PSLICE()
                                 << "bag-of-cells invalid root index " << idx);
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
      assert(false);
      index_ptr = data.substr(info.index_offset).ubegin();
      // TODO: should we validate index here
    } else {
      index_ptr = nullptr;
      unsigned long long cur = 0;
      custom_index.reserve(info.cell_count);

      auto cells_slice = data.substr(info.data_offset, info.data_size);

      for (int i = 0; i < info.cell_count; i++) {
        CellSerializationInfo cell_info;
        auto status = cell_info.init(cells_slice, info.ref_byte_size);
        if (status.is_error()) {
          return td::Status::Error(
              PSLICE() << "invalid bag-of-cells failed to deserialize cell #"
                       << i << " " << status.error());
        }
        cells_slice = cells_slice.substr(cell_info.end_offset);
        cur += cell_info.end_offset;
        custom_index.push_back(cur);
        // std::cerr << "cur is " << cur << std::endl;
      }
      // TODO: it is OK to not reach the end
      // if (!cells_slice.empty()) {
      //   return td::Status::Error(PSLICE()
      //                            << "invalid bag-of-cells last cell #"
      //                            << info.cell_count - 1 << ": end offset "
      //                            << cur << " is different from total data
      //                            size "
      //                            << info.data_size);
      // }
    }
    unsigned long long meta_start = info.data_offset;
    unsigned long long meta_end = info.data_offset + custom_index.back();
    unsigned long long cell_data_start = meta_end;
    unsigned long long cell_data_end = info.data_offset + info.data_size;

    auto cells_slice = data.substr(meta_start, meta_end - meta_start);
    auto data_slice =
        data.substr(cell_data_start, cell_data_end - cell_data_start);

    std::vector<td::Ref<vm::DataCell>> cell_list;
    cell_list.reserve(cell_count);
    std::array<td::Ref<vm::Cell>, 4> refs_buf;

    auto topol_order = get_topol_order(cells_slice, cell_count).move_as_ok();
    std::vector<int> idx_map(cell_count, -1);
    std::vector<std::pair<unsigned long long, unsigned long long>>
        cell_data_interval;
    unsigned long long current_cell_data_offset = 0;

    for (int i = 0; i < info.cell_count; i++) {
      CellSerializationInfo cell_info;
      cell_info.init(cells_slice.substr((i ? custom_index[i - 1] : 0)),
                     info.ref_byte_size);
      unsigned long long cell_data_start = current_cell_data_offset;
      unsigned long long cell_data_end = cell_data_start + cell_info.data_len;
      cell_data_interval.emplace_back(cell_data_start, cell_data_end);
      current_cell_data_offset = cell_data_end;
    }

    for (auto &&idx : topol_order) {
      auto r_cell = deserialize_cell(
          idx_map, idx, cells_slice, data_slice, cell_data_interval[idx].first,
          cell_list, info.has_cache_bits ? &cell_should_cache : nullptr);
      if (r_cell.is_error()) {
        return td::Status::Error(
            PSLICE() << "invalid bag-of-cells failed to deserialize cell #"
                     << idx << " " << r_cell.error());
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
          return td::Status::Error(PSLICE() << "invalid bag-of-cells cell #"
                                            << idx << " has wrong cache flag "
                                            << stored_should_cache);
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

  td::uint64 compute_sizes(int mode, int &r_size, int &o_size) {
    int rs = 0, os = 0;
    if (!root_count || !data_bytes) {
      r_size = o_size = 0;
      return 0;
    }
    while (cell_count >= (1LL << (rs << 3))) {
      rs++;
    }
    td::uint64 hashes = (((mode & Mode::WithTopHash) ? top_hashes : 0) +
                         ((mode & Mode::WithIntHashes) ? int_hashes : 0)) *
                        (vm::Cell::hash_bytes + vm::Cell::depth_bytes);
    td::uint64 data_bytes_adj =
        data_bytes + (unsigned long long)int_refs * rs + hashes;
    td::uint64 max_offset =
        (mode & Mode::WithCacheBits) ? data_bytes_adj * 2 : data_bytes_adj;
    while (max_offset >= (1ULL << (os << 3))) {
      os++;
    }
    if (rs > 4 || os > 8) {
      r_size = o_size = 0;
      return 0;
    }
    r_size = rs;
    o_size = os;
    return data_bytes_adj;
  }

  std::size_t estimate_serialized_size(int mode) {
    if ((mode & Mode::WithCacheBits) && !(mode & Mode::WithIndex)) {
      info.invalidate();
      return 0;
    }
    auto data_bytes_adj =
        compute_sizes(mode, info.ref_byte_size, info.offset_byte_size);
    if (!data_bytes_adj) {
      info.invalidate();
      return 0;
    }
    info.valid = true;
    info.has_crc32c = mode & Mode::WithCRC32C;
    info.has_index = mode & Mode::WithIndex;
    info.has_cache_bits = mode & Mode::WithCacheBits;
    info.root_count = root_count;
    info.cell_count = cell_count;
    info.absent_count = dangle_count;
    int crc_size = info.has_crc32c ? 4 : 0;
    info.roots_offset =
        4 + 1 + 1 + 3 * info.ref_byte_size + info.offset_byte_size;
    info.index_offset =
        info.roots_offset + info.root_count * info.ref_byte_size;
    info.data_offset = info.index_offset;
    if (info.has_index) {
      info.data_offset += (long long)cell_count * info.offset_byte_size;
    }
    info.magic = Info::boc_generic;
    info.data_size = data_bytes_adj;
    info.total_size = info.data_offset + data_bytes_adj + crc_size;
    auto res = td::narrow_cast_safe<size_t>(info.total_size);
    if (res.is_error()) {
      return 0;
    }
    return res.ok();
  }

  td::Result<td::BufferSlice> serialize_to_slice(int mode) {
    std::size_t size_est = estimate_serialized_size(mode);
    if (!size_est) {
      return td::Status::Error("no cells to serialize to this bag of cells");
    }
    td::BufferSlice res(size_est);
    auto buff = const_cast<unsigned char *>(
        reinterpret_cast<const unsigned char *>(res.data()));

    TRY_RESULT(size, serialize_to(buff, res.size(), mode));

    //FIXME
    return std::move(res);
    if (size == res.size()) {
      return std::move(res);
    } else {
      return td::Status::Error("error while serializing a bag of cells: actual "
                               "serialized size differs from estimated");
    }
  }

  td::Result<std::size_t> serialize_to(unsigned char *buffer,
                                       std::size_t buff_size, int mode) {
    std::size_t size_est = estimate_serialized_size(mode);
    if (!size_est || size_est > buff_size) {
      return 0;
    }

    vm::boc_writers::BufferWriter writer{buffer, buffer + size_est};
    // serialize_to_impl_only_meta(writer, mode);
    return serialize_to_impl_only_data(writer, mode);
  }

  template <typename WriterT>
  td::Result<std::size_t> serialize_to_impl_only_data(WriterT &writer,
                                                      int mode) {
    for (int i = 0; i < cell_count; ++i) {
      const auto &dc_info = cell_list_[cell_count - 1 - i];
      const td::Ref<vm::DataCell> &dc = dc_info.dc_ref;
      bool with_hash = (mode & Mode::WithIntHashes) && !dc_info.wt;
      if (dc_info.is_root_cell && (mode & Mode::WithTopHash)) {
        with_hash = true;
      }
      unsigned char buf[256];
      auto myDc = reinterpret_cast<const MyDataCell *>(dc.get());
      int s = myDc->serialize(buf, 256, with_hash, false);
      writer.store_bytes(buf, s);
    }

    return writer.position();
  }

  // serialized_boc#672fb0ac has_idx:(## 1) has_crc32c:(## 1)
  //  has_cache_bits:(## 1) flags:(## 2) { flags = 0 }
  //  size:(## 3) { size <= 4 }
  //  off_bytes:(## 8) { off_bytes <= 8 }
  //  cells:(##(size * 8))
  //  roots:(##(size * 8))
  //  absent:(##(size * 8)) { roots + absent <= cells }
  //  tot_cells_size:(##(off_bytes * 8))
  //  index:(cells * ##(off_bytes * 8))
  //  cell_data:(tot_cells_size * [ uint8 ])
  //  = BagOfCells;
  // Changes in this function may require corresponding changes in
  // crypto/vm/large-boc-serializer.cpp
  template <typename WriterT>
  td::Result<std::size_t> serialize_to_impl_only_meta(WriterT &writer,
                                                      int mode) {
    auto store_ref = [&](unsigned long long value) {
      writer.store_uint(value, info.ref_byte_size);
    };
    auto store_offset = [&](unsigned long long value) {
      writer.store_uint(value, info.offset_byte_size);
    };

    writer.store_uint(info.magic, 4);

    td::uint8 byte{0};
    if (info.has_index) {
      byte |= 1 << 7;
    }
    if (info.has_crc32c) {
      byte |= 1 << 6;
    }
    if (info.has_cache_bits) {
      byte |= 1 << 5;
    }
    // 3, 4 - flags
    if (info.ref_byte_size < 1 || info.ref_byte_size > 7) {
      return 0;
    }
    byte |= static_cast<td::uint8>(info.ref_byte_size);
    writer.store_uint(byte, 1);

    writer.store_uint(info.offset_byte_size, 1);
    store_ref(cell_count);
    store_ref(root_count);
    store_ref(0);
    store_offset(info.data_size);
    for (const auto &root_info : roots) {
      int k = cell_count - 1 - root_info.idx;
      DCHECK(k >= 0 && k < cell_count);
      store_ref(k);
    }
    DCHECK(writer.position() == info.index_offset);
    DCHECK((unsigned)cell_count == cell_list_.size());
    if (info.has_index) {
      std::size_t offs = 0;
      for (int i = cell_count - 1; i >= 0; --i) {
        const td::Ref<vm::DataCell> &dc = cell_list_[i].dc_ref;
        bool with_hash = (mode & Mode::WithIntHashes) && !cell_list_[i].wt;
        if (cell_list_[i].is_root_cell && (mode & Mode::WithTopHash)) {
          with_hash = true;
        }
        offs += dc->get_serialized_size(with_hash) +
                dc->size_refs() * info.ref_byte_size;
        auto fixed_offset = offs;
        if (info.has_cache_bits) {
          fixed_offset = offs * 2 + cell_list_[i].should_cache;
        }
        store_offset(fixed_offset);
      }
      DCHECK(offs == info.data_size);
    }
    DCHECK(writer.position() == info.data_offset);
    size_t keep_position = writer.position();
    for (int i = 0; i < cell_count; ++i) {
      const auto &dc_info = cell_list_[cell_count - 1 - i];
      const td::Ref<vm::DataCell> &dc = dc_info.dc_ref;
      bool with_hash = (mode & Mode::WithIntHashes) && !dc_info.wt;
      if (dc_info.is_root_cell && (mode & Mode::WithTopHash)) {
        with_hash = true;
      }
      // unsigned char buf[256];
      // int s = dc->serialize(buf, 256, with_hash);
      // writer.store_bytes(buf, s);
      auto myDc = reinterpret_cast<const MyDataCell *>(dc.get());
      unsigned char buf[2];
      auto first_two_meta = myDc->get_first_two_meta(with_hash);
      buf[0] = first_two_meta.first;
      buf[1] = first_two_meta.second;
      writer.store_bytes(buf, 2);

      DCHECK(dc->size_refs() == dc_info.ref_num);
      // std::cerr << (dc_info.is_special() ? '*' : ' ') << i << '<' <<
      // (int)dc_info.wt << ">:";
      for (unsigned j = 0; j < dc_info.ref_num; ++j) {
        int k = cell_count - 1 - dc_info.ref_idx[j];
        DCHECK(k > i && k < cell_count);
        store_ref(k);
        // std::cerr << ' ' << k;
      }
      // std::cerr << std::endl;
    }
    writer.chk();
    DCHECK(writer.position() - keep_position == info.data_size);
    DCHECK(writer.remaining() == (info.has_crc32c ? 4 : 0));
    if (info.has_crc32c) {
      unsigned crc = writer.get_crc32();
      writer.store_uint(td::bswap32(crc), 4);
    }
    DCHECK(writer.empty());
    return writer.position();
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

td::Result<td::BufferSlice> my_std_boc_serialize(td::Ref<vm::Cell> root,
                                                 int mode = 0) {
  if (root.is_null()) {
    return td::Status::Error(
        "cannot serialize a null cell reference into a bag of cells");
  }
  vm::BagOfCells boc;
  boc.add_root(std::move(root));
  auto res = boc.import_cells();

  auto myBoc = reinterpret_cast<MyBagOfCells *>(&boc);

  if (res.is_error()) {
    return res.move_as_error();
  }
  return myBoc->serialize_to_slice(mode);
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
    return td::Status::Error(
        "bag of cells is expected to have exactly one root");
  }
  auto root = boc.get_root_cell();
  if (root.is_null()) {
    return td::Status::Error("bag of cells has null root cell (?)");
  }
  if (!allow_nonzero_level && root->get_level() != 0) {
    return td::Status::Error("bag of cells has a root with non-zero level");
  }
  return std::move(root);
}

td::BufferSlice compress(td::Slice data) {
  td::Ref<vm::Cell> root = vm::std_boc_deserialize(data).move_as_ok();
  return lzma_compress(my_std_boc_serialize(root, 0).move_as_ok());
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
