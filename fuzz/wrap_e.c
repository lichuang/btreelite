/*
** fuzz/wrap_e.c -- libFuzzer adapter for the Target E harness
** (two handles, one file, contention).
*/
#include <stdint.h>
#include <stddef.h>

int btreeliteFuzzEOneInput(const uint8_t *a, size_t n, const char *zName);

int LLVMFuzzerTestOneInput(const uint8_t *a, size_t n){
  return btreeliteFuzzEOneInput(a, n, "libfuzzer");
}