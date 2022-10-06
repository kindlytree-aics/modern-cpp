//基于现代C++无锁编程实现的线程池
//其中有界队列可以参考之前的有界队列的无锁实现一文
//apollo源码位置：cyber\base\thread_pool.h
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "bounded_queue.h"

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t thread_num, std::size_t max_task_num = 1000);
  // 函数模板，而不是类模板。
  // template<> 部分: template <typename F, typename... Args>。typename... Args代表接受多个参数。
  //"typename"是一个C++程序设计语言中的关键字。相当用于泛型编程时是另一术语"class"的同义词
  // 形参表： (F&& f, Args&&… args)。&&是C++ 11新特性，代表右值引用。
  // -> std::future< typename std::result_of< F(Args…)>::type>。
  // 这个->符号其实用到了C++ 11中的lamda表达式，后面的内容代表函数的返回类型。 
  // 声明了一个名为Enqueue()的函数模板，它的模板类型为class F以及多个其他类型Args，
  // 它的形参是一个F&&类型的f以及多个Args&&类型的args，
  // 最后这个函数返回类型是std::future< typename std::result_of < F(Args…)>::type >。
  // 对于这个冗长的返回类型，又可以继续分析：
  // std::future在前面提到过了，它本身是一个模板，包含在 < future>中。
  // 通过std::future可以返回这个A类型的异步任务的结果。
  // std::result_of<F(Args...)>::type就是这段代码中的A类型。result_of获取了someTask的执行结果的类型。
  // F(Args…)_就是这段代码的someTask，即函数F(Args…)。
  // 所以最后这个模板函数Enqueue()的返回值类型就是F(Args…)的异步执行结果类型。

  template <typename F, typename... Args>
  auto Enqueue(F&& f, Args&&... args)
      -> std::future<typename std::result_of<F(Args...)>::type>;

  ~ThreadPool();

 private:
  //worker线程的个数
  std::vector<std::thread> workers_;
  //每个任务
  //在C语言的时代，我们可以使用函数指针来吧一个函数作为参数传递，
  //这样我们就可以实现回调函数的机制。
  //到了C++11以后在标准库里引入了std::function模板类，这个模板概括了函数指针的概念
  //BoundedQueue沿用了已有的无锁实现的有界队列
  BoundedQueue<std::function<void()>> task_queue_;
  std::atomic_bool stop_;
};

inline ThreadPool::ThreadPool(std::size_t threads, std::size_t max_task_num)
    : stop_(false) {
  if (!task_queue_.Init(max_task_num, new BlockWaitStrategy())) {
    throw std::runtime_error("Task queue init failed.");
  }
  //reserve的作用是更改vector的容量（capacity），使vector至少可以容纳n个元素。
  //如果n大于vector当前的容量，reserve会对vector进行扩容。
  //其他情况下都不会重新分配vector的存储空间
  workers_.reserve(threads);
  for (size_t i = 0; i < threads; ++i) {
    //emplace_back支持in-place construction，
    //也就是说emplace_back(10, “test”)可以只调用一次constructor，
    //而push_back(MyClass(10, “test”))必须多一次构造和析构
    //emplace_back括号的内容是
    //emplace_back()与push_back()类似，但是前者更适合用来传递对象，
    //因为它可以避免对象作为参数被传递时在拷贝成员上的开销。
    //这里emplace_back()了一个lambda表达式[this]{…}。
    //lambda表达式本身代表一个匿名函数(即没有函数名的函数)，
    //通常格式为[捕获列表](参数列表)->return 返回类型{函数体}。
    //而在本代码中的lambda表达式是作为一个线程放入workers[]中。
    //这个线程是个while循环。
    workers_.emplace_back([this] {
      while (!stop_) {
        std::function<void()> task;
        if (task_queue_.WaitDequeue(&task)) {
          task();
        }
      }
    });
  }
}

// before using the return value, you should check value.valid()
template <typename F, typename... Args>
auto ThreadPool::Enqueue(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type> {
  //using … = typename …; 功能类似typedef。
  //将return_type声明为一个result_of< F(Args…)>::type类型，即函数F(Args…)的返回值类型。
  using return_type = typename std::result_of<F(Args...)>::type;

  //packaged_task : 把任务打包,这里打包的是return_type
  //packaged_task类模板也是定义于future头文件中，它包装任何可调用 (Callable) 目标，
  //包括函数、 lambda 表达式、 bind 表达式或其他函数对象，使得能异步调用它，
  //其返回值或所抛异常被存储于能通过,std::future 对象访问的共享状态中。 
  //简言之，将一个普通的可调用函数对象转换为异步执行的任务
  //packaged_task<int(int,int)> task([](int a, int b){ return a + b;});
  //bind : 绑定函数f, 参数为args…
  //forward : 使()转化为<>相同类型的左值或右值引用

  auto task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));

  std::future<return_type> res = task->get_future();

  // don't allow enqueueing after stopping the pool
  if (stop_) {
    return std::future<return_type>();
  }
  task_queue_.Enqueue([task]() { (*task)(); });
  return res;
};

// the destructor joins all threads
inline ThreadPool::~ThreadPool() {
  if (stop_.exchange(true)) {
    return;
  }
  task_queue_.BreakAllWait();
  for (std::thread& worker : workers_) {
    worker.join();
  }
}
