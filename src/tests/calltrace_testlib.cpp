// Test target for calltrace_test: a DLL whose classes carry RTTI (like the engine modules), with methods that cover
// the calling conventions and control flow the call tracer must pass through unchanged.

#include <windows.h>

#include <cstdarg>

#include "calltrace_testlib.h"

namespace {

struct Target : ITarget {
    int Add(int a, int b) override { return a + b; }
    float Scale(float x, float y) override { return x * y; }
    double Mul(double a, double b) override { return a * b; }
    long long Wide(long long a, int b) override { return a * b; }
    Vec3 Make(float a, float b, float c) override { return Vec3{a, b, c}; }
    int Sum(int n, ...) override {
        va_list v;
        va_start(v, n);
        int s = 0;
        for (int i = 0; i < n; ++i) s += va_arg(v, int);
        va_end(v);
        return s;
    }
    int Call(ICallback* cb, int depth) override { return cb->Run(depth) + 1; }
    int Throw(int code) override {
        RaiseException(0xE0000000u | unsigned(code & 0xFFFF), 0, 0, nullptr);
        return -1;
    }
    // calls from inside the module, through the vtable (volatile: no devirtualization)
    __declspec(noinline) int Outer(int x) override {
        ITarget* volatile self = this;
        return self->Inner(x) * 2;
    }
    int Inner(int x) override { return x + 1; }
    __declspec(noinline) int Tail(int x) override {
        ITarget* volatile self = this;
        return self->Inner(x + 1);  // usually compiled as a tail jump
    }
};

}  // namespace

extern "C" __declspec(dllexport) ITarget* CreateTarget() { return new Target; }
