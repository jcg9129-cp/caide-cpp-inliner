#include <bits/stdc++.h>


long long factorial(int n) { long long r = 1; for (int i = 2; i <= n; ++i) r *= i; return r; }
int main() { return (int)(factorial(5) % 1000); }

