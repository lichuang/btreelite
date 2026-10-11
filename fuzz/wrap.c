/*
** fuzz/wrap.c -- libFuzzer adapter for the Target A harness.
** (Target B/C wrap the same btreeliteFuzzOneInput.)
*/
#include <stdint.h>
#include <stddef.h>

int btreeliteFuzzOneInput(const uint8_t *a, size_t n, const char *zName);

int LLVMFuzzerTestOneInput(const uint8_t *a, size_t n){
  return btreeliteFuzzOneInput(a, n, "libfuzzer");
}