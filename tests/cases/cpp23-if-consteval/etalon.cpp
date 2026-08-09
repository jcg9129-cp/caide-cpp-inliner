// C++23 if consteval
constexpr int f(int x) {
    if consteval {
        return x + 1;
    } else {
        return x - 1;
    }
}


int main() {
    return f(10);
}

