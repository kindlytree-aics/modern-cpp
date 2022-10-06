## 线程的等待策略
- BlockWaitStrategy 基于mutex的wait notity机制
- SleepWaitStrategy 基于线程睡眠的机制
- YieldWaitStrategy 基于线程切换让出的机制
- TimeoutBlockWaitStrategy 基于timeout的有限时间等待策略
![线程的状态图](./figures/thread_state.png)

### BlockWaitStrategy
该策略利用条件变量std::condition_variable来阻塞线程，然后等待通知将其唤醒。我们可以通过某个函数判断是否符合某种条件来决定是阻塞线程等待通知还是唤醒线程。阻塞状态的线程跟可运行状态的线程不是同一个队列。内核总是从可运行状态的队列里面取出优先级最高的去运行。当阻塞的线程阻塞完了重新置为可运行状态的才会跟其它线程抢占CPU。

```
//当前线程在cv_.wait(lock); 这句时会hang住，
//在等待其他线程notify(NotifyOne或者BreakAllWait)的时候再唤醒执行后面的return true后返回
//condition_variable的等待函数wait会无条件的阻塞当前线程然后等待通知，
//此时对象lock已经成功获取了锁。等待时会调用lock.unlock()释放锁，使其它线程可以获取锁。
//一旦得到通知(由其他线程显式地通知)，函数就会释放阻塞并调用lock.lock()获取锁执行后面的操作
class BlockWaitStrategy : public WaitStrategy {
 public:
  BlockWaitStrategy() {}
  void NotifyOne() override { cv_.notify_one(); }

  bool EmptyWait() override {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock);
    return true;
  }

  void BreakAllWait() override { cv_.notify_all(); }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
};
```

### SleepWaitStrategy
std::this_thread::sleep_for()是让当前休眠”指定的一段”时间.
sleep_for()也可以起到 std::this_thread::yield()相似的作用, (即:当前线程在休眠期间, 自然不会与其他线程争抢CPU时间片)但两者的使用目的是大不相同的:

```
class SleepWaitStrategy : public WaitStrategy {
 public:
  SleepWaitStrategy() {}
  explicit SleepWaitStrategy(uint64_t sleep_time_us)
      : sleep_time_us_(sleep_time_us) {}

  bool EmptyWait() override {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_time_us_));
    return true;
  }

  void SetSleepTimeMicroSeconds(uint64_t sleep_time_us) {
    sleep_time_us_ = sleep_time_us;
  }

 private:
  uint64_t sleep_time_us_ = 10000;
};  
```

### YieldWaitStrategy
std::this_thread::yield() 的目的是避免一个线程(that should be used in a case where you are in a busy waiting state)频繁与其他线程争抢CPU时间片, 从而导致多线程处理性能下降.
std::this_thread::yield() 是让当前线程让渡出自己的CPU时间片(给其他线程使用)

```
class YieldWaitStrategy : public WaitStrategy {
 public:
  YieldWaitStrategy() {}
  bool EmptyWait() override {
    std::this_thread::yield();
    return true;
  }
};
```
### BusySpinWaitStrategy
不释放 CPU 的基础上等待事件的技术,相当于通过轮询进行，具体可以参考bounded_queue测试用例示例：
```
class BusySpinWaitStrategy : public WaitStrategy {
 public:
  BusySpinWaitStrategy() {}
  bool EmptyWait() override { return true; }
};
```

### TimeoutBlockWaitStrategy
执行wait_for函数时，会自动释放当前锁，将当前线程阻塞，将其放入到等待线程的队列，该线程会在其他线程执行notify_all或者notify_one函数时唤醒，又或者时时间超时被唤醒，当唤醒后，重新获取锁，wait_for函数退出。

Atomically releases lock, blocks the current executing thread, and adds it to the list of threads waiting on *this. The thread will be unblocked when notify_all() or notify_one() is executed, or when the relative timeout rel_time expires. It may also be unblocked spuriously. When unblocked, regardless of the reason, lock is reacquired and wait_for() exits.

```
class TimeoutBlockWaitStrategy : public WaitStrategy {
 public:
  TimeoutBlockWaitStrategy() {}
  explicit TimeoutBlockWaitStrategy(uint64_t timeout)
      : time_out_(std::chrono::milliseconds(timeout)) {}

  void NotifyOne() override { cv_.notify_one(); }

  bool EmptyWait() override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cv_.wait_for(lock, time_out_) == std::cv_status::timeout) {
      return false;
    }
    return true;
  }

  void BreakAllWait() override { cv_.notify_all(); }

  void SetTimeout(uint64_t timeout) {
    time_out_ = std::chrono::milliseconds(timeout);
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::chrono::milliseconds time_out_;
};
```

### Reference
- apollo代码，代码位置：`cyber\base\wait_strategy.h`
- 实验代码：`.\src\bounded_queue.cpp`