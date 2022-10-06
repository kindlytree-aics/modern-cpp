### 线程安全的有界队列及其无锁实现:
该实现用定长数组实现有界对立，维护head，tail以及当前commit的位置信息，通过atomic操作来保证线程安全
- 无锁编程
- apollo代码位置：`cyber\base\bounded_queue.h`
- 实验代码：`.\src\bounded_queue.cpp`

```
template <typename T>
class BoundedQueue {
 public:
  using value_type = T;
  using size_type = uint64_t;

 public:
  BoundedQueue() {}
  BoundedQueue& operator=(const BoundedQueue& other) = delete;
  BoundedQueue(const BoundedQueue& other) = delete;
  ~BoundedQueue();
  bool Init(uint64_t size);
  bool Init(uint64_t size, WaitStrategy* strategy);
  bool Enqueue(const T& element);
  bool Enqueue(T&& element);
  bool WaitEnqueue(const T& element);
  bool WaitEnqueue(T&& element);
  bool Dequeue(T* element);
  bool WaitDequeue(T* element);
  uint64_t Size();
  bool Empty();
  //WaitStrategy参考线程的等待策略一节相关内容
  void SetWaitStrategy(WaitStrategy* WaitStrategy);
  void BreakAllWait();
  uint64_t Head() { return head_.load(); }
  uint64_t Tail() { return tail_.load(); }
  uint64_t Commit() { return commit_.load(); }

 private:
  uint64_t GetIndex(uint64_t num);

  //C++11提供了关键字alignas来设置数据的对齐方式：
  //#define CACHELINE_SIZE 64
  //alignas关键字用来设置内存中对齐方式，最小是8字节对齐，可以是16，32，64，128等。
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> head_ = {0};
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> tail_ = {1};
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> commit_ = {1};
  // alignas(CACHELINE_SIZE) std::atomic<uint64_t> size_ = {0};
  uint64_t pool_size_ = 0;
  //数据池是指针类型
  T* pool_ = nullptr;
  std::unique_ptr<WaitStrategy> wait_strategy_ = nullptr;
  volatile bool break_all_wait_ = false;
};

//析构函数
template <typename T>
BoundedQueue<T>::~BoundedQueue() {
  if (wait_strategy_) {
    BreakAllWait();
  }
  //由于对象是placement new生成的，不会自动释放（对象实际上是借用别人的空间），
  //所以必须显示的调用类的析构函数，如本例中的pool_[i].~T()，但是内存并不会被释放，以便其他对象的构造。
  //最终整块区域的内存释放交给了std::free
  if (pool_) {
    for (uint64_t i = 0; i < pool_size_; ++i) {
      pool_[i].~T();
    }
    std::free(pool_);
  }
}

template <typename T>
inline bool BoundedQueue<T>::Init(uint64_t size) {
  return Init(size, new SleepWaitStrategy());
}

template <typename T>
bool BoundedQueue<T>::Init(uint64_t size, WaitStrategy* strategy) {
  // Head and tail each occupy a space
  //池子的大小为size+2，队列头和尾各占一个空间；
  pool_size_ = size + 2;
  //开始分配空间；在内存的动态存储区中分配n个长度为size的连续空间，
  //函数返回一个指向分配起始地址的指针；如果分配不成功，返回NULL。
  //数组元素初始化为0(malloc不初始化，里边数据是随机的垃圾数据).
  pool_ = reinterpret_cast<T*>(std::calloc(pool_size_, sizeof(T)));
  if (pool_ == nullptr) {
    return false;
  }
  //placement new构造对象都是在一个预先准备好了的内存缓冲区中进行，不需要查找内存，
  //内存分配的时间是常数；而且不会出现在程序运行中途出现内存不足的异常。
  for (uint64_t i = 0; i < pool_size_; ++i) {
    new (&(pool_[i])) T();
  }
  wait_strategy_.reset(strategy);
  return true;
}

//通过原子操作实现线程安全的入队操作
template <typename T>
bool BoundedQueue<T>::Enqueue(const T& element) {
  uint64_t new_tail = 0;
  uint64_t old_commit = 0;
  uint64_t old_tail = tail_.load(std::memory_order_acquire);
  //do while循环先执行do，然后再判断while里面的条件，起码要执行一次
  do {
    new_tail = old_tail + 1;
    //如果队列已满，不能进行入队操作，直接返回false
    if (GetIndex(new_tail) == GetIndex(head_.load(std::memory_order_acquire))) {
      return false;
    }
  } while (!tail_.compare_exchange_weak(old_tail, new_tail,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed));
    //tail_为原子变量，将当前的tail_的值和old_tail进行比较，如果相等，则tail_更新为new_tail
    //返回true,!操作返回fasle，使得跳出循环，开始下面的入队操作
    //否则，如果tail_的值和old_tail不相等（将old_tail更新为当前的tail_值），
    //说明其他线程已经做了do里边的操作并且tail_值已经更新，已经抢先入队
    //这时返回false,!操作返回true，继续下一次执行do里面的操作，等待入队的时机（或队列已满返回false）
  //在old_tail的位置入队，old_tail可能在上面的循环了进行了多次的累加
  //和程序入口的old_tail可能已经不同了
  pool_[GetIndex(old_tail)] = element;
  do {
    old_commit = old_tail;
  } while (cyber_unlikely(!commit_.compare_exchange_weak(
      old_commit, new_tail, std::memory_order_acq_rel,
      std::memory_order_relaxed)));
  //commit_为原子变量，将commit_和old_commit进行比较，
  //如果相等，则commit_更新为new_tail，返回true，!操作返回false，跳出循环实现了入队
  //如果不相等，则old_commit更新为当前的commit_值（不过do里边又会覆盖为old_tail的值）
  //因此commit_的值是完全根据入队的顺序进行递增的，不同线程根据入队的循序依次跳出该循环
  //哪个线程先完成入队操作，哪个线程先跳出该while循环

  wait_strategy_->NotifyOne();
  return true;
}

//和上面的功能类似，不过这里用到了&&和move操作，可以参考专门的主题章节
template <typename T>
bool BoundedQueue<T>::Enqueue(T&& element) {
  uint64_t new_tail = 0;
  uint64_t old_commit = 0;
  uint64_t old_tail = tail_.load(std::memory_order_acquire);
  do {
    new_tail = old_tail + 1;
    if (GetIndex(new_tail) == GetIndex(head_.load(std::memory_order_acquire))) {
      return false;
    }
  } while (!tail_.compare_exchange_weak(old_tail, new_tail,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed));
  pool_[GetIndex(old_tail)] = std::move(element);
  do {
    old_commit = old_tail;
  } while (cyber_unlikely(!commit_.compare_exchange_weak(
      old_commit, new_tail, std::memory_order_acq_rel,
      std::memory_order_relaxed)));
  wait_strategy_->NotifyOne();
  return true;
}

template <typename T>
bool BoundedQueue<T>::Dequeue(T* element) {
  uint64_t new_head = 0;
  uint64_t old_head = head_.load(std::memory_order_acquire);
  do {
    new_head = old_head + 1;
    //队列已经空队列，返回false
    if (new_head == commit_.load(std::memory_order_acquire)) {
      return false;
    }
    *element = pool_[GetIndex(new_head)];
  } while (!head_.compare_exchange_weak(old_head, new_head,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed));
    //head_原子变量,和old_head比较，
    //如果相等，则更新为new_head并返回true，!操作取反返回false，退出循环
    //如果不等，则说明其他线程已经取走了当前的head元素，将old_head更新为head_值
    //并进入下一次do里面的操作
  return true;
}

//这里实现了等待机制，如果队列未满，则立马插入返回，否则进入空等状态
//知道队列不再满后再插入，或者等待超时返回。
template <typename T>
bool BoundedQueue<T>::WaitEnqueue(const T& element) {
  while (!break_all_wait_) {
    if (Enqueue(element)) {
      return true;
    }
    if (wait_strategy_->EmptyWait()) {
      continue;
    }
    // wait timeout
    break;
  }

  return false;
}

template <typename T>
bool BoundedQueue<T>::WaitEnqueue(T&& element) {
  while (!break_all_wait_) {
    if (Enqueue(std::move(element))) {
      return true;
    }
    if (wait_strategy_->EmptyWait()) {
      continue;
    }
    // wait timeout
    break;
  }

  return false;
}

//这里实现了等待机制，如果队列未空，则立马取回队首元素返回，否则进入空等状态
//知道队列不再空后再取回队首元素，或者等待超时返回。
template <typename T>
bool BoundedQueue<T>::WaitDequeue(T* element) {
  while (!break_all_wait_) {
    if (Dequeue(element)) {
      return true;
    }
    if (wait_strategy_->EmptyWait()) {
      continue;
    }
    // wait timeout
    break;
  }
  return false;
}

template <typename T>
inline uint64_t BoundedQueue<T>::Size() {
  return tail_ - head_ - 1;
}

template <typename T>
inline bool BoundedQueue<T>::Empty() {
  return Size() == 0;
}

template <typename T>
inline uint64_t BoundedQueue<T>::GetIndex(uint64_t num) {
  return num - (num / pool_size_) * pool_size_;  // faster than %
}

template <typename T>
inline void BoundedQueue<T>::SetWaitStrategy(WaitStrategy* strategy) {
  wait_strategy_.reset(strategy);
}

template <typename T>
inline void BoundedQueue<T>::BreakAllWait() {
  break_all_wait_ = true;
  wait_strategy_->BreakAllWait();
}
```