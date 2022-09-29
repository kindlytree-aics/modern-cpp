#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <iostream>

#include "wait_strategy.h"

#define CACHELINE_SIZE  64
#define cyber_likely(x) (__builtin_expect((x), 1))
#define cyber_unlikely(x) (__builtin_expect((x), 0))

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
  void SetWaitStrategy(WaitStrategy* WaitStrategy);
  void BreakAllWait();
  uint64_t Head() { return head_.load(); }
  uint64_t Tail() { return tail_.load(); }
  uint64_t Commit() { return commit_.load(); }

 private:
  uint64_t GetIndex(uint64_t num);

  alignas(CACHELINE_SIZE) std::atomic<uint64_t> head_ = {0};
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> tail_ = {1};
  alignas(CACHELINE_SIZE) std::atomic<uint64_t> commit_ = {1};
  // alignas(CACHELINE_SIZE) std::atomic<uint64_t> size_ = {0};
  uint64_t pool_size_ = 0;
  T* pool_ = nullptr;
  std::unique_ptr<WaitStrategy> wait_strategy_ = nullptr;
  volatile bool break_all_wait_ = false;
};

template <typename T>
BoundedQueue<T>::~BoundedQueue() {
  if (wait_strategy_) {
    BreakAllWait();
  }
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
  pool_size_ = size + 2;
  pool_ = reinterpret_cast<T*>(std::calloc(pool_size_, sizeof(T)));
  if (pool_ == nullptr) {
    return false;
  }
  for (uint64_t i = 0; i < pool_size_; ++i) {
    new (&(pool_[i])) T();
  }
  wait_strategy_.reset(strategy);
  return true;
}

template <typename T>
bool BoundedQueue<T>::Enqueue(const T& element) {
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
  pool_[GetIndex(old_tail)] = element;
  do {
    old_commit = old_tail;
  } while (cyber_unlikely(!commit_.compare_exchange_weak(
      old_commit, new_tail, std::memory_order_acq_rel,
      std::memory_order_relaxed)));
  wait_strategy_->NotifyOne();
  return true;
}

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
    if (new_head == commit_.load(std::memory_order_acquire)) {
      return false;
    }
    *element = pool_[GetIndex(new_head)];
  } while (!head_.compare_exchange_weak(old_head, new_head,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed));
  return true;
}

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

int main()
{
  BoundedQueue<int> queue;
  queue.Init(10);
  std::atomic_int count = {0};
  std::thread threads[48];
  for (int i = 0; i < 48; ++i) {
    if (i % 4 == 0) {
      threads[i] = std::thread([&]() {
        for (int j = 0; j < 10000; ++j) {
          //队列满了返回false，count不进行计数
          if (queue.Enqueue(j)) {
            count++;
          }
        }
      });
    } else if (i % 4 == 1) {
      threads[i] = std::thread([&]() {
        for (int j = 0; j < 10000; ++j) {
          //如果队列满了，当前线程等待，
          //等待结束后再尝试入队操作，如果成功入队，则下面的语句返回true
          if (queue.WaitEnqueue(j)) {
            count++;
          }
        }
      });
    } else if (i % 4 == 2) {
      threads[i] = std::thread([&]() {
        for (int j = 0; j < 10000; ++j) {
          int value = 0;
          //如果队列非空，则出队列
          //否则下面的语句返回false
          if (queue.Dequeue(&value)) {
            count--;
          }
        }
      });
    } else {
      threads[i] = std::thread([&]() {
        for (int j = 0; j < 10000; ++j) {
          int value = 0;
          //如果队列非空，则出队列
          //否则等待，唤醒后再去队列尝试取值          
          if (queue.WaitDequeue(&value)) {
            count--;
          }
        }
      });
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  //唤醒所有等待线程，并且设置相关while循环不成立的条件，未后面的join线程退出做准备
  queue.BreakAllWait();
  for (int i = 0; i < 48; ++i) {
    threads[i].join();
  }
  int count_sz = count.load(), queue_sz = queue.Size();
  std::cout << " count_sz: " << count_sz << " queue_sz: " << queue_sz << std::endl;
}