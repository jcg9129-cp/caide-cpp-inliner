int state = 0;

int initialize() {
    state += 2;
    return state;
}

int dynamicGlobal = initialize();
thread_local int dynamicThread = initialize();

struct StaticOwner {
    static int dynamicMember;
};

int StaticOwner::dynamicMember = initialize();

struct Finalizer {
    ~Finalizer() { state += 10; }
};

Finalizer finalizers[2] = {};
thread_local Finalizer threadFinalizer = {};

__attribute__((constructor)) void startup() { state += 1; }
__attribute__((destructor)) void shutdown() { state += 1; }
__attribute__((used)) int retainedConstant = 42;
__attribute__((used)) int retainedFunction() { return 42; }

template<class T>
struct UsedTemplate {
};

int main() {
    UsedTemplate<int> object;
    (void)object;
    return state == 5 ? 0 : 1;
}
