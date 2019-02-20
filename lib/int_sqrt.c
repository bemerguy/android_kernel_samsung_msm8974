#include <linux/kernel.h>
#include <linux/export.h>
unsigned long int_sqrt(unsigned long n) {
  unsigned long s, t;

#define sqrtBit(k) \
  t = s+(1UL<<(k-1)); t <<= k+1; if (n >= t) { n -= t; s |= 1UL<<k; }

  s = 0UL;
#ifdef __alpha
  if (n >= 1UL<<62) { n -= 1UL<<62; s = 1UL<<31; }
  sqrtBit(30); sqrtBit(29); sqrtBit(28); sqrtBit(27); sqrtBit(26);
  sqrtBit(25); sqrtBit(24); sqrtBit(23); sqrtBit(22); sqrtBit(21);
  sqrtBit(20); sqrtBit(19); sqrtBit(18); sqrtBit(17); sqrtBit(16);
  sqrtBit(15);
#else
  if (n >= 1UL<<30) { n -= 1UL<<30; s = 1UL<<15; }
#endif
  sqrtBit(14); sqrtBit(13); sqrtBit(12); sqrtBit(11); sqrtBit(10);
  sqrtBit(9); sqrtBit(8); sqrtBit(7); sqrtBit(6); sqrtBit(5);
  sqrtBit(4); sqrtBit(3); sqrtBit(2); sqrtBit(1);
  if (n > s<<1) s |= 1UL;

#undef sqrtBit
  return s;
}

EXPORT_SYMBOL(int_sqrt);
