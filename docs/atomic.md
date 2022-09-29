### 原子类的使用
原子变量是可以执行原子操作的变量，原子操作是不可以被打断的。同时，原子操作会遵守相应的内存模型（Sequential Consistency, Acquire-Release, Relaxed Semantics 等)，给线程调度的顺序增加一定的限制，从而减少由线程调度的随机性带来的未定义的行为。

### CAS(Compare And Swap)
CAS为一个原子操作，其可以见当前的原子变量的值和期望值进行比较，根据比较的结果进行对应的操作
当前值与期望值(expect)相等时，修改当前值为设定值(desired)，返回true
当前值与期望值(expect)不等时，将期望值(expect)修改为当前值，返回false
具体使用见后面的代码说明
std::atomic compare_exchange_weak和compare_exchange_strong


### 应用示例：
- [基于原子操作实现的读写锁](./atomic_rw_lock.md)