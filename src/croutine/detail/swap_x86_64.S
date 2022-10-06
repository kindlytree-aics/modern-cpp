/*
asm code
调用SwapContext()函数前物理的rsp寄存器指向的是系统栈，
经过下面汇编ctx_swap函数一系列pushq，
会将当前的这些callee-saved registers全都压到栈中保存起来，
然后movq %rsp, (%rdi)语句将rsp存到main_stack_变量中。
这样后面切回来时才能把这些上下文恢复回来。
calling convention规定，寄存器rdi放的第一个参数。
对于Resume()函数中调用的ctx_swap函数来说，
第一个参数实际为main_stack_。
同理，由于寄存器rsi放的是第二个参数，这里为&(context_->sp)。
因此，下一条语句movq (%rsi), %rsp是将之前设好的RoutineContext中的sp设到寄存器rsp上，
这样栈就已经切过来了。然后是一系列popq，
将协程栈上对应寄存器的内容搬到物理寄存器上。
最后一条ret语句，就是把CRoutineEntry()函数的地址弹出来，然后跳过去执行。
看CRoutineEntry()的实现知道它会调用当时构建CRoutine时用户传入的函数。这样就开始执行用户逻辑了。
*/
.globl ctx_swap
.type  ctx_swap, @function
ctx_swap:
      pushq %rdi
      pushq %r12
      pushq %r13
      pushq %r14
      pushq %r15
      pushq %rbx
      pushq %rbp
      movq %rsp, (%rdi)

      movq (%rsi), %rsp
      popq %rbp
      popq %rbx
      popq %r15
      popq %r14
      popq %r13
      popq %r12
      popq %rdi
      ret
