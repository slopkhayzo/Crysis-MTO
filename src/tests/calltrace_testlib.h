#pragma once
// Interface shared by calltrace_test.exe and calltrace_testlib.dll.

struct Vec3 {
    float x, y, z;
};

struct ICallback {
    virtual int Run(int depth) = 0;
};

struct ITarget {
    virtual int Add(int a, int b) = 0;
    virtual float Scale(float x, float y) = 0;                // x86: returned in st0
    virtual double Mul(double a, double b) = 0;
    virtual long long Wide(long long a, int b) = 0;           // x86: edx:eax
    virtual Vec3 Make(float a, float b, float c) = 0;         // hidden return pointer
    virtual int Sum(int n, ...) = 0;                          // variadic: cdecl on x86
    virtual int Call(ICallback* cb, int depth) = 0;           // calls back into the caller's module
    virtual int Throw(int code) = 0;                          // raises an SEH exception the caller catches
    virtual int Outer(int x) = 0;                             // calls Inner from inside the module
    virtual int Inner(int x) = 0;
    virtual int Tail(int x) = 0;                              // tail-calls Inner
    virtual ~ITarget() = default;
};

enum TargetSlot { kAdd, kScale, kMul, kWide, kMake, kSum, kCall, kThrow, kOuter, kInner, kTail, kSlots };
