/*
** fuzz/replay.c -- deterministic replay runner: run the Target A
** harness over a directory of action streams.  No fuzzing-engine
** dependency, so the seed corpus stays a permanent regression asset.
**
** Each stream runs twice: the second run re-checks that no state leaks
** across iterations (same rules as libFuzzer's persistent mode).
**
** Usage: fuzz_replay [dir-or-file ...]   (default: fuzz/seeds)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int btreeliteFuzzOneInput(const uint8_t *a, size_t n, const char *zName);
int btreeliteFuzzFileOneInput(const uint8_t *a, size_t n, const char *zName);
int btreeliteFuzzEOneInput(const uint8_t *a, size_t n, const char *zName);

/* The directory name picks the target: "oomseeds" = Target C (OOM
** injection; the stream's first byte is the K of the faulting
** allocator), "fileseeds" = Target B (hostile files, needs
** fuzz/files/ regenerated first), "eseeds" = Target E (contention),
** anything else = Target A.  The dispatch peeks full tags in the order
** shown: "oomseeds" is checked first, and note "fileseeds" contains
** "eseeds" as a substring, which is why Target B is checked before
** Target E. */
static const char *zDirTag;

static int runFile(const char *zPath, const char *zName, uint32_t *pnChk){
  FILE *f = fopen(zPath, "rb");
  uint8_t *a;
  size_t n, got;
  int rc;
  int (*oneInput)(const uint8_t*, size_t, const char*) =
      (zDirTag && strstr(zDirTag, "fileseeds")!=0) ? btreeliteFuzzFileOneInput
      : (zDirTag && strstr(zDirTag, "eseeds")!=0) ? btreeliteFuzzEOneInput
      : btreeliteFuzzOneInput;
  if( f==0 ) return 1;
  fseek(f, 0, SEEK_END);
  n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  a = (uint8_t*)malloc(n ? n : 1);
  if( a==0 ){ fclose(f); return 1; }
  got = fread(a, 1, n, f);
  fclose(f);
  if( got!=n ){ free(a); return 1; }
  rc = oneInput(a, got, zName);
  *pnChk += (uint32_t)(rc < 0 ? 0 : rc);
  free(a);
  return 0;
}

static int runPath(const char *zPath, int *pnFile, uint32_t *pnChk){
  struct stat st;
  zDirTag = zPath;
  if( stat(zPath, &st) ) return 1;
  if( S_ISDIR(st.st_mode) ){
    DIR *dp = opendir(zPath);
    struct dirent *de;
    char sub[2048];
    if( dp==0 ) return 1;
    while( (de = readdir(dp)) ){
      const char *z = de->d_name;
      size_t lz = strlen(z);
      if( z[0]=='.' ) continue;
      if( lz<4 || strcmp(z+lz-4, ".bin") ) continue;
      snprintf(sub, sizeof sub, "%s/%s", zPath, z);
      if( runFile(sub, z, pnChk) ){        /* run twice: the second run
        ** is the cross-iteration-hygiene check (persistent-mode rules) */
        fprintf(stderr, "replay: %s FAILED first pass\n", z);
        return 1;
      }
      if( runFile(sub, z, pnChk) ){
        fprintf(stderr, "replay: %s FAILED second pass\n", z);
        return 1;
      }
      printf("  ok %s\n", z);
      fflush(stdout);
      (*pnFile)++;
    }
    closedir(dp);
    return 0;
  }
  if( runFile(zPath, zPath, pnChk) ) return 1;
  printf("  ok %s\n", zPath);
  (*pnFile)++;
  return 0;
}

int main(int argc, char **argv){
  int i, nFile = 0, bad = 0;
  uint32_t nChk = 0;
  if( argc>1 ){
    for(i=1; i<argc; i++) if( runPath(argv[i], &nFile, &nChk) ) bad = 1;
  }else{
    if( runPath("fuzz/seeds", &nFile, &nChk) ) bad = 1;
  }
  printf("replay: %d streams, %u contract checks, %d failures\n",
         nFile, nChk, bad);
  return bad;
}