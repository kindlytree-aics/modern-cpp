
#include "bounded_queue.h"

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