### 基于协程的任务调度之任务编排调度

#### 任务编排模式下基于协程的任务调度

##### 任务编排模式下的上下文
- 任务编排模式下的上下文`ChoreographyContext`,代码可以参考`cyber\scheduler\policy\choreography_context.h`, 
任务编排模式的上下文中定义了几个关键的数据结构，代码及说明如下：

```
class ChoreographyContext : public ProcessorContext {
 public:
  bool RemoveCRoutine(uint64_t crid);
  std::shared_ptr<CRoutine> NextRoutine() override;

  bool Enqueue(const std::shared_ptr<CRoutine>&);
  void Notify();
  void Wait() override;
  void Shutdown() override;

 private:
  std::mutex mtx_wq_;
  std::condition_variable cv_wq_;
  int notify = 0;

  AtomicRWLock rq_lk_;
  //multimap结构定义了基于优先级的协程队列，在这里key为线程的优先级，value为任务协程
  //1、multimap是关联式容器，它按特定的次序（按照key来比较）存储由键key和值value组合而成的元素,多个键值对之间的key可以重复
  //2、在multimap中，键值key通常用于排序和唯一标识元素，值value中存储与键key关联的内容。
  //3、multimap中通过键值访问单个元素比unordered_multimap容器慢，使用迭代器直接遍历multimap的元素可以得到关于key有序的序列
  //4、multimap的底层通常是平衡二叉搜索树（红黑树）
  //5、multimap和map的唯一区别是multimap中的key可以重复，而map的key是唯一的
  std::multimap<uint32_t, std::shared_ptr<CRoutine>, std::greater<uint32_t>>
      cr_queue_;
};

```

同时，其调度方法的实现代码如下
```
std::shared_ptr<CRoutine> ChoreographyContext::NextRoutine() {
  if (cyber_unlikely(stop_.load())) {
    return nullptr;
  }
  //优先访问优先级高的协程，cr_queue_结构为线程私有，在不同的线程间没有以静态变量的方式共享
  ReadLockGuard<AtomicRWLock> lock(rq_lk_);
  for (auto it : cr_queue_) {
    auto cr = it.second;
    if (!cr->Acquire()) {
      continue;
    }

    if (cr->UpdateState() == RoutineState::READY) {
      return cr;
    }
    cr->Release();
  }
  return nullptr;
}
```
##### 任务编排模式下的调度器`SchedulerChoreography`
任务编排模式的调度策略可以参考配置文件`cyber\conf\example_sched_choreography.conf`,
任务编排模式的调度器的代码为`cyber\scheduler\policy\scheduler_choreography.cc`, 其中初始化中调用了`CreateProcessor`函数创建线程，并根据配置设置线程的亲和度和策略。

`cyber\conf\example_sched_choreography.conf`中示例如下：下面对一些关键字段做一下说明：

choreography策略，主要是对主链路上的任务进行编排（choreography开头的配置），将非主链路的任务放到线程池中由classic策略（pool开头的配置）执行，choreography策略中classic线程池存在的意义：主链路的任务执行先后关系比较明确，但是存在一些其他链路的任务在不清楚前后拓扑关系的情况下，或者说未被编排的任务（包括Async创建的异步task），会被放到classic线程池中执行。
关于配置属性：

choreography_processor_policy和choreography_processor_prio是设置编排线程的调度策略和优先级，这里设置SCHED_FIFO是为了保证主链路能够及时抢占cpu执行， pool_processor_policy和pool_processor_prio是设置classic线程的调度策略和优先级。


下面的配置中A、B、C、D是主链路任务，都设置了processor属性，那么A、B在0号cpu上执行，C、D在1号cpu上执行。在同一个核上，A和B还设置了优先级，所以优先执行B，再执行A。
没有配置processor属性的任务E以及没有出现在task配置中的任务如F，则默认进入classic线程池中执行。
考虑到任务优先级、执行时长、频率与调度之间的关系，任务编排有如下几个依据：
- 同一个path的任务尽量编排在同一个processor，如果processor负载过高，将部分任务拆分到另外其他processor
- 同一个path上的任务从开始到结束，优先级逐级升高
- 不同path上的任务尽量不混排
- 高频&短耗时任务尽量排放同一processor

```
    choreography_conf {
        choreography_processor_num: 8
        choreography_affinity: "range"
        choreography_cpuset: "0-7"
        choreography_processor_policy: "SCHED_FIFO" # policy: SCHED_OTHER,SCHED_RR,SCHED_FIFO
        choreography_processor_prio: 10

        pool_processor_num: 8
        pool_affinity: "range"
        pool_cpuset: "16-23"
        pool_processor_policy: "SCHED_OTHER"
        pool_processor_prio: 0

        tasks: [
            {
                name: "A"
                processor: 0
                prio: 1
            },
            {
                name: "B"
                processor: 0
                prio: 2
            },
            {
                name: "C"
                processor: 1
                prio: 1
            },
            {
                name: "D"
                processor: 1
                prio: 2
            },
            {
                name: "E"
            }
        ]
    }
```
其创建配置线程的方式和经典方式也有区别，表现在如下的代码里

```
void SchedulerChoreography::CreateProcessor() {
  //定义任务编排的线程生成方法
  for (uint32_t i = 0; i < proc_num_; i++) {
    auto proc = std::make_shared<Processor>();
    auto ctx = std::make_shared<ChoreographyContext>();

    proc->BindContext(ctx);
    SetSchedAffinity(proc->Thread(), choreography_cpuset_,
                     choreography_affinity_, i);
    SetSchedPolicy(proc->Thread(), choreography_processor_policy_,
                   choreography_processor_prio_, proc->Tid());
    pctxs_.emplace_back(ctx);
    processors_.emplace_back(proc);
  }
  //其他任务采用经典调度策略用线程池的方式去调度协程
  for (uint32_t i = 0; i < task_pool_size_; i++) {
    auto proc = std::make_shared<Processor>();
    auto ctx = std::make_shared<ClassicContext>();

    proc->BindContext(ctx);
    SetSchedAffinity(proc->Thread(), pool_cpuset_, pool_affinity_, i);
    SetSchedPolicy(proc->Thread(), pool_processor_policy_, pool_processor_prio_,
                   proc->Tid());
    pctxs_.emplace_back(ctx);
    processors_.emplace_back(proc);
  }
}
```

其分发任务的代码如下,`DispatchTask`函数一般在`Component`组件的初始化里调用，将组件的处理任务封装为协程调用调度器的任务分发函数将其
推送到对应的优先级队列里，这里和经典调度策略的不同点表现在会将配置了`processor`的协程放到特定的队列里，其他的协程任务分配到经典调度定义的共享队列里。
```
bool SchedulerChoreography::DispatchTask(const std::shared_ptr<CRoutine>& cr) {
  // we use multi-key mutex to prevent race condition
  // when del && add cr with same crid
  MutexWrapper* wrapper = nullptr;
  if (!id_map_mutex_.Get(cr->id(), &wrapper)) {
    {
      std::lock_guard<std::mutex> wl_lg(cr_wl_mtx_);
      if (!id_map_mutex_.Get(cr->id(), &wrapper)) {
        wrapper = new MutexWrapper();
        id_map_mutex_.Set(cr->id(), wrapper);
      }
    }
  }
  std::lock_guard<std::mutex> lg(wrapper->Mutex());

  // Assign sched cfg to tasks according to configuration.
  if (cr_confs_.find(cr->name()) != cr_confs_.end()) {
    ChoreographyTask taskconf = cr_confs_[cr->name()];
    cr->set_priority(taskconf.prio());

    if (taskconf.has_processor()) {
      cr->set_processor_id(taskconf.processor());
    }
  }

  {
    WriteLockGuard<AtomicRWLock> lk(id_cr_lock_);
    if (id_cr_.find(cr->id()) != id_cr_.end()) {
      return false;
    }
    id_cr_[cr->id()] = cr;
  }

  // Enqueue task.
  // 如果给协程任务分配了线程（cpu）id，则放入到线程对应的上下文的队列里，这些协程任务和线程绑定到了特定的cpu上执行。
  uint32_t pid = cr->processor_id();
  if (pid < proc_num_) {
    // Enqueue task to Choreo Policy.
    static_cast<ChoreographyContext*>(pctxs_[pid].get())->Enqueue(cr);
  } else {
    // Check if task prio is reasonable.
    if (cr->priority() >= MAX_PRIO) {
      AWARN << cr->name() << " prio great than MAX_PRIO.";
      cr->set_priority(MAX_PRIO - 1);
    }

    cr->set_group_name(DEFAULT_GROUP_NAME);

    // Enqueue task to pool runqueue.
    // 否则，将其他的任务放到ClassicContext上下文共享的协程队列里
    {
      WriteLockGuard<AtomicRWLock> lk(
          ClassicContext::rq_locks_[DEFAULT_GROUP_NAME].at(cr->priority()));
      ClassicContext::cr_group_[DEFAULT_GROUP_NAME]
          .at(cr->priority())
          .emplace_back(cr);
    }
  }
  return true;
}
```

##### FAQ
1、在编排任务调度的策略下，有基于线程和cpu绑定的任务编排调度，也有基于线程池的经典任务调度，两者怎么混用在一起的？
两者能一起调度运行在于不同的Context的NextRoutine函数实现机制的不同，Processor::Run()是线程运行的函数，在该函数内部会调用
context_->NextRoutine()语句去调度基于协程的任务执行，在定义了任务编排的Processor里，线程（协程）和cpu已经绑定，只调度该cpu
下的协程任务，在其他的Processor里，会采用经典调度方法去在共享的线程池（协程池）了去调度协程。
```
std::shared_ptr<CRoutine> ClassicContext::NextRoutine() {
  if (cyber_unlikely(stop_.load())) {
    return nullptr;
  }

  for (int i = MAX_PRIO - 1; i >= 0; --i) {
    ReadLockGuard<AtomicRWLock> lk(lq_->at(i));
    for (auto& cr : multi_pri_rq_->at(i)) {
      if (!cr->Acquire()) {
        continue;
      }

      if (cr->UpdateState() == RoutineState::READY) {
        return cr;
      }

      cr->Release();
    }
  }

  return nullptr;
}

std::shared_ptr<CRoutine> ChoreographyContext::NextRoutine() {
  if (cyber_unlikely(stop_.load())) {
    return nullptr;
  }

  ReadLockGuard<AtomicRWLock> lock(rq_lk_);
  for (auto it : cr_queue_) {
    auto cr = it.second;
    if (!cr->Acquire()) {
      continue;
    }

    if (cr->UpdateState() == RoutineState::READY) {
      return cr;
    }
    cr->Release();
  }
  return nullptr;
}

```
