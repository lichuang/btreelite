/*
** fuzz/wrap_oom.c -- libFuzzer adapter for the Target C harness
** (deterministic OOM injection over the Target A action stream).
*/
#include <stdint.h>
#include <stddef.h>

int btreeliteFuzzOomOneInput(const uint8_t *a, size_t n, const char *zName);

int LLVMFuzzerTestOneInput(const uint8_t *a, size_t n){
  return btreeliteFuzzOomOneInput(a, n, "libfuzzer");
}