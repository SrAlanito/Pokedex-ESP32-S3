#include <stdio.h>
#include <stdint.h>

#include "../src/mail_codec.h"

int main() {
  const uint8_t pass[20] = {5, 0, 0, 0, 0, 1, 0, 0, 0, 0x3c, 0xff, 0, 1, 0, 1, 0, 9, 8, 55, 0};
  char out[25];
  wonderEncode(pass, out);
  fwrite(out, 1, 24, stdout);
  return 0;
}
