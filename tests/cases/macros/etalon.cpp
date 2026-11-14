#define MACRO_TO_KEEP


int a() {

    return 2;

}

int b() {
#if __cplusplus > 1
    return 1;
#else
    return 2;
#endif
}

#define bar 1
int c() {
#if 1 + 1 > 1*(bar+bar)
    return 1;
#else
    return 2;
#endif
}

int main() {
    return a() + b() + c();
}


