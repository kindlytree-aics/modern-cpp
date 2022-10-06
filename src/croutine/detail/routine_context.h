#include <cstdlib>
#include <cstring>
#include <iostream>

extern "C" {
extern void ctx_swap(void**, void**) asm("ctx_swap");
};

constexpr size_t STACK_SIZE = 2 * 1024 * 1024;
#if defined __aarch64__
constexpr size_t REGISTERS_SIZE = 160;
#else
constexpr size_t REGISTERS_SIZE = 56;
#endif

//成员sp对应寄存器rsp。
//成员sp指向的成员stack中的位置从低往高依次为7个寄存器大小的空间及CRoutineEntry函数指针
//7个寄存器分别对应rdi, rbx, rbp, r12-r15。
//根据x86_64平台的ABI calling convention，
//rbx, rbp, r12-r15是callee-saved registers。
//也就是说被调用者有责任保存它们，
//以保证它们在函数过程（对调用者来说，SwapContext()的过程就像一个普通函数调用）中值不变。
//为什么对应rdi位置放的是CRoutine的指针，因为calling convention中规定rdi放被调用函数的第一个参数。
//而CRoutineEntry()第一个参数正是CRoutine指针。

typedef void (*func)(void*);
struct RoutineContext {
  char stack[STACK_SIZE];
  char* sp = nullptr;
#if defined __aarch64__
} __attribute__((aligned(16)));
#else
};
#endif

void MakeContext(const func& f1, const void* arg, RoutineContext* ctx);

inline void SwapContext(char** src_sp, char** dest_sp) {
  ctx_swap(reinterpret_cast<void**>(src_sp), reinterpret_cast<void**>(dest_sp));
}
