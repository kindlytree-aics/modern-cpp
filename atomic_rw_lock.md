### 原子读写锁的实现:
atomic是c++11标准,在gcc编译的时候必须加入std=c++11选项才能正确编译
- 无锁编程
- 代码位置：`cyber\base\atomic_rw_lock.h`
- 和自旋锁（spinlock）的关系(TO be added)

### CAS(Compare And Swap)
CAS为一个原子操作，其可以见当前的原子变量的值和期望值进行比较，根据比较的结果进行对应的操作
当前值与期望值(expect)相等时，修改当前值为设定值(desired)，返回true
当前值与期望值(expect)不等时，将期望值(expect)修改为当前值，返回false
具体使用见后面的代码说明
std::atomic compare_exchange_weak和compare_exchange_strong

```
class AtomicRWLock {

  //声明两个友元类，这两个类主要是利用C++的RAII机制对加锁和开锁的封装
  //这两个类主要调用本类的四个privte接口：ReadLock，WriteLock，ReadUnlock，WriteUnlock
  //就像C11中的std::mutex和std::lock_guard之间的关系
  friend class ReadLockGuard<AtomicRWLock>;
  friend class WriteLockGuard<AtomicRWLock>;

 public:
  static const int32_t RW_LOCK_FREE = 0;//RW_LOCK_FREE表示目前没人占用
  static const int32_t WRITE_EXCLUSIVE = -1;//WRITE_EXCLUSIVE则表示当前锁被一个写的操作占用
  static const uint32_t MAX_RETRY_TIMES = 5;
  //尝试获取锁的时候连续尝试次数，就像自旋锁那样，连续失败MAX_RETRY_TIMES次则会让出线程的执行
  AtomicRWLock() {}
  explicit AtomicRWLock(bool write_first) : write_first_(write_first) {}

 private:
  // all these function only can used by ReadLockGuard/WriteLockGuard;
  void ReadLock();
  void WriteLock();

  void ReadUnlock();
  void WriteUnlock();

  AtomicRWLock(const AtomicRWLock&) = delete;
  AtomicRWLock& operator=(const AtomicRWLock&) = delete;
  std::atomic<uint32_t> write_lock_wait_num_ = {0};
  std::atomic<int32_t> lock_num_ = {0};
  bool write_first_ = true;
};

inline void AtomicRWLock::ReadLock() {
  uint32_t retry_times = 0;
  //读取原子状态
  int32_t lock_num = lock_num_.load();
  if (write_first_) {//如果优先写操作
    do {
      //如果在写状态（lock_num_为WRITE_EXCLUSIVE），
      //或者write_lock_wait_num_>0(写锁已经调用write_lock_wait_num_.fetch_add(1);)
      //即写锁已经开始争抢资源
      //则当前的读锁不能继续争抢，需要进入轮询状态
      while (lock_num < RW_LOCK_FREE || write_lock_wait_num_.load() > 0) {
        if (++retry_times == MAX_RETRY_TIMES) {
          // saving cpu,当前线程让出cpu
          //std::this_thread::yield(); 是将当前线程所抢到的CPU”时间片A”让渡给其他线程
          //(其他线程会争抢”时间片A”,注意: 此时”当前线程”不参与争抢).
          //等到其他线程使用完”时间片A”后, 再由操作系统调度, 当前线程再和其他线程一起开始抢CPU时间片.
          std::this_thread::yield();
          retry_times = 0;
        }
        //不断监测lock_num_的状态，一直到其>=RW_LOCK_FREE(0),
        //写状态解锁会执行WriteUnlock，调用lock_num_.fetch_add(1);
        lock_num = lock_num_.load();
      }
    } while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
      //当前锁退出了写状态，但有可能有多个线程会争抢读操作
      //while里边的操作是当前原子lock_num_，compare_exchange_weak第一个参数是期望值，原子本身的值是当前值
      //case 1：当前值与期望值(expect)相等时，修改当前值为设定值(desired)，返回true, !为false，while循环结束
      //如lock_num_当前为0,lock_num也为0，则将lock_num_设为+1(lock_num + 1)
      //case 2：当前值与期望值(expect)不相等时(可能有其他的线程修改了lock_num_的值)，
      //这时需要将期望值(expect)修改为当前值，返回false，继续循环
      //其他的线程修改了lock_num_的情况有
      //1、其他读线程成功申请了锁，
      //这时再执行一次compare_exchange_weak操作很有可能就跳出循环,因为读操作可以共享，可以多个线程申请到读锁
      //2、这时刚好没有读线程，且写线程申请到了写锁资源，lock_num_已经为WRITE_EXCLUSIVE，同样需要再进入循环   
  } else {
    do {
      while (lock_num < RW_LOCK_FREE) {
        if (++retry_times == MAX_RETRY_TIMES) {
          // saving cpu
          std::this_thread::yield();
          retry_times = 0;
        }
        lock_num = lock_num_.load();
      }
    } while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
  }
}

inline void AtomicRWLock::WriteLock() {
  int32_t rw_lock_free = RW_LOCK_FREE;
  uint32_t retry_times = 0;
  write_lock_wait_num_.fetch_add(1);
  while (!lock_num_.compare_exchange_weak(rw_lock_free, WRITE_EXCLUSIVE,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
    // rw_lock_free will change after CAS fail, so init agin
    //如果当前有读操作在进行，lock_num_ >0,当前值与期望值(expect)不相等时
    //这时需要将期望值(expect)修改为当前值，返回false，!为true，
    //此时rw_lock_free值已经修改为lock_num_的值，需要再改回来，同时进行多次轮训
    //如果达到轮询次数的上限，则让出cpu，当前线程hang住等待下次调度
    //若当前已经没有读操作在进行了(读锁已经释放),则lock_num_为0，
    //此时当前值与期望值(rw_lock_free)相等时（均为0），修改当前值为设定值(desired，即为WRITE_EXCLUSIVE)，返回true
    //再加！操作，跳出while循环
    rw_lock_free = RW_LOCK_FREE;
    if (++retry_times == MAX_RETRY_TIMES) {
      // saving cpu
      std::this_thread::yield();
      retry_times = 0;
    }
  }
  //write_lock_wait_num_减一操作，变为0，此时进入写操作（lock_num_已经为WRITE_EXCLUSIVE），
  write_lock_wait_num_.fetch_sub(1);
}

//一旦读锁拿到锁则意味着lock_num_是正整数，也就是+1,当他释放该锁的时候将lock_num_减去1很好理解
inline void AtomicRWLock::ReadUnlock() { lock_num_.fetch_sub(1); }

inline void AtomicRWLock::WriteUnlock() { lock_num_.fetch_add(1); }
```

### Reference
- https://github.com/ApolloAuto/apollo
- https://blog.csdn.net/liujiayu2/article/details/124732353
