struct A {
    int a, b, c, d;
    bool e, f, g;
    static long h;
};

long A::h = 1;

int main() {
    A x;
    x.b = x.c = A::h;
    x.g = false;
}
