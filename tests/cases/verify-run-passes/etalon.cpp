#include <cstdio>

int main() {
    int a, b;
    if (std::scanf("%d%d", &a, &b) != 2)
        return 1;
    std::printf("%d\n", a + b);
}
