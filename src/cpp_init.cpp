extern "C" {
void __cpp_init(void) {}
}

namespace {
struct CppRuntimeInit {
    CppRuntimeInit() {}
    ~CppRuntimeInit() {}
} cpp_init_dummy;
}
