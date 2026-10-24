/****************************************************************************
 * firmware/apps/vmflow/vmflow_hmac.c
 *
 * HMAC-SHA256 with the device passkey: rpc-auth.c of the ESP-IDF firmware
 * (mbedtls md there). SHA-256 is FIPS 180-4, HMAC is RFC 2104; both are
 * small enough to carry here instead of a crypto library.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmflow.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SHA256_BLOCK  64

#define ROR(x, n)     (((x) >> (n)) | ((x) << (32 - (n))))

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sha256_s
{
  uint32_t h[8];
  uint8_t block[SHA256_BLOCK];
  size_t used;
  uint64_t total;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint32_t g_k[64] =
{
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sha256_compress(FAR struct sha256_s *s, FAR const uint8_t *p)
{
  uint32_t w[64];
  uint32_t a[8];
  uint32_t t1;
  uint32_t t2;
  int i;

  for (i = 0; i < 16; i++)
    {
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
             (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }

  for (i = 16; i < 64; i++)
    {
      w[i] = w[i - 16] + w[i - 7] +
             (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
             (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    }

  memcpy(a, s->h, sizeof(a));

  for (i = 0; i < 64; i++)
    {
      t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) +
           ((a[4] & a[5]) ^ (~a[4] & a[6])) + g_k[i] + w[i];
      t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) +
           ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));

      memmove(&a[1], &a[0], 7 * sizeof(a[0]));
      a[4] += t1;
      a[0]  = t1 + t2;
    }

  for (i = 0; i < 8; i++)
    {
      s->h[i] += a[i];
    }
}

static void sha256_init(FAR struct sha256_s *s)
{
  static const uint32_t h0[8] =
  {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  };

  memcpy(s->h, h0, sizeof(h0));
  s->used  = 0;
  s->total = 0;
}

static void sha256_update(FAR struct sha256_s *s, FAR const void *data,
                          size_t len)
{
  FAR const uint8_t *p = data;
  size_t n;

  s->total += len;

  while (len > 0)
    {
      n = SHA256_BLOCK - s->used;
      if (n > len)
        {
          n = len;
        }

      memcpy(s->block + s->used, p, n);
      s->used += n;
      p       += n;
      len     -= n;

      if (s->used == SHA256_BLOCK)
        {
          sha256_compress(s, s->block);
          s->used = 0;
        }
    }
}

static void sha256_final(FAR struct sha256_s *s, FAR uint8_t *out)
{
  uint64_t bits = s->total * 8;
  uint8_t pad = 0x80;
  uint8_t len[8];
  int i;

  sha256_update(s, &pad, 1);

  pad = 0;
  while (s->used != SHA256_BLOCK - 8)
    {
      sha256_update(s, &pad, 1);
    }

  for (i = 0; i < 8; i++)
    {
      len[i] = bits >> (56 - 8 * i);
    }

  sha256_update(s, len, 8);

  for (i = 0; i < 8; i++)
    {
      out[4 * i]     = s->h[i] >> 24;
      out[4 * i + 1] = s->h[i] >> 16;
      out[4 * i + 2] = s->h[i] >> 8;
      out[4 * i + 3] = s->h[i];
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void vmflow_hmac(FAR const char *key, FAR const void *msg, size_t len,
                 FAR uint8_t *out)
{
  uint8_t k[SHA256_BLOCK];
  uint8_t pad[SHA256_BLOCK];
  uint8_t inner[VMFLOW_HMAC_LEN];
  struct sha256_s s;
  size_t klen = strlen(key);
  int i;

  memset(k, 0, sizeof(k));
  if (klen > SHA256_BLOCK)
    {
      sha256_init(&s);
      sha256_update(&s, key, klen);
      sha256_final(&s, k);
    }
  else
    {
      memcpy(k, key, klen);
    }

  for (i = 0; i < SHA256_BLOCK; i++)
    {
      pad[i] = k[i] ^ 0x36;
    }

  sha256_init(&s);
  sha256_update(&s, pad, sizeof(pad));
  sha256_update(&s, msg, len);
  sha256_final(&s, inner);

  for (i = 0; i < SHA256_BLOCK; i++)
    {
      pad[i] = k[i] ^ 0x5c;
    }

  sha256_init(&s);
  sha256_update(&s, pad, sizeof(pad));
  sha256_update(&s, inner, sizeof(inner));
  sha256_final(&s, out);
}

/* "<msg>:<hmac hex>", rpc_sign_text() in the ESP-IDF firmware */

void vmflow_sign(FAR const char *key, FAR const char *msg, FAR char *out,
                 size_t outlen)
{
  uint8_t mac[VMFLOW_HMAC_LEN];
  char hex[2 * VMFLOW_HMAC_LEN + 1];
  int i;

  vmflow_hmac(key, msg, strlen(msg), mac);
  for (i = 0; i < VMFLOW_HMAC_LEN; i++)
    {
      snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    }

  snprintf(out, outlen, "%s:%s", msg, hex);
}

bool vmflow_verify(FAR const char *key, FAR const char *msg, size_t len,
                   FAR const char *hex)
{
  uint8_t mac[VMFLOW_HMAC_LEN];
  char expect[2 * VMFLOW_HMAC_LEN + 1];
  uint8_t diff = 0;
  int i;

  if (strlen(hex) != 2 * VMFLOW_HMAC_LEN)
    {
      return false;
    }

  vmflow_hmac(key, msg, len, mac);
  for (i = 0; i < VMFLOW_HMAC_LEN; i++)
    {
      snprintf(expect + 2 * i, 3, "%02x", mac[i]);
    }

  /* Constant time: the comparison does not stop at the first mismatch */

  for (i = 0; i < 2 * VMFLOW_HMAC_LEN; i++)
    {
      diff |= expect[i] ^ hex[i];
    }

  return diff == 0;
}
