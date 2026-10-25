/*
** fuzz/wrap_combo.c -- libFuzzer adapter for the P4 combination harness
** (Target A's stateful actions on Target B's damaged files).
*/
#include <stdint.h>
#include <stddef.h>

int btreeliteFuzzComboOneInput(const uint8_t *a, size_t n, const char *zName);

int LLVMFuzzerTestOneInput(const uint8_t *a, size_t n){
  return btreeliteFuzzComboOneInput(a, n, "libfuzzer");
}