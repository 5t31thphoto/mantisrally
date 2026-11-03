// The packed art unpacks to exactly what the art tools made: dumps the unpacked bytes for the comparison.
#include <cstdio>
#include "art.h"
#include "art_packed.h"
using namespace mantis;
int main() {
  if (!art::artInit()) { printf("artInit FAILED\n"); return 1; }
  FILE* f = fopen("/tmp/art_unpacked.bin", "wb");
  fwrite(art::VEH_ART[0].pal, 1, artpack::RAW_SIZE, f);      // the copy starts at the first item
  fclose(f);
  printf("unpacked %u bytes\n", (unsigned)artpack::RAW_SIZE);
  return 0;
}
