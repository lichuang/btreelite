/*
** fuzz/wrap_file.c -- libFuzzer adapter for the Target B harness
** (hostile files; bases from fuzz/files/).
*/
#include <stdint.h>
#include <stddef.h>

int btreeliteFuzzFileOneInput(const uint8_t *a, size_t n, const char *zName);

int LLVMFuzzerTestOneInput(const uint8_t *a, size_t n){
  return btreeliteFuzzFileOneInput(a, n, "libfuzzer");
}