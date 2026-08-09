#include <cstdio>


int countBits(unsigned x) { return __builtin_popcount(x); }
int main() { return countBits(255u); }

