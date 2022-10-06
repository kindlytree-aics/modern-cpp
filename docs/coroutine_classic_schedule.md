### 基于协程的任务调度

#### 协程的载体线程
协程需要在线程里边运行，协程可以理解为轻量级的用户线程，一个线程中可以运行多个协程任务。
在Apollo里，Processor类（代码位置：cyber\scheduler\processor.cc）对线程进行了封装，其Run函数
里的while循环通过不断调用`context_->NextRoutine()`来调度协程并运行，其中`context_`是类`ProcessorContext`
的实例，类`ProcessorContext`又有`ClassicContext`和`ChoreographyContext`两个子类，分别为经典模式和
编排模式的上下文，重载了`NextRoutine()`虚函数，实现了不同的任务调度。

```
void Processor::Run() {
  tid_.store(static_cast<int>(syscall(SYS_gettid)));
  AINFO << "processor_tid: " << tid_;
  snap_shot_->processor_id.store(tid_);

  while (cyber_likely(running_.load())) {
    if (cyber_likely(context_ != nullptr)) {
      //根据具体的context的种类获取下一个协程，即为协程的调度
      auto croutine = context_->NextRoutine();
      if (croutine) {
        snap_shot_->execute_start_time.store(cyber::Time::Now().ToNanosecond());
        snap_shot_->routine_name = croutine->name();
        croutine->Resume();
        croutine->Release();
      } else {
        snap_shot_->execute_start_time.store(0);
        context_->Wait();
      }
    } else {
      std::unique_lock<std::mutex> lk(mtx_ctx_);
      cv_ctx_.wait_for(lk, std::chrono::milliseconds(10));
    }
  }
}

```

#### 经典模式下基于协程的任务调度

##### 经典模式下的上下文
- 经典模式下的上下文`ClassicContext`,代码可以参考`cyber\scheduler\policy\classic_context.h`, 
经典模式的上下文中定义了几个关键的数据结构，代码及说明如下：

```
//基于unordered_map数据结构，unordered_map是一种基于hash函数的存储方式，效率比较高
using CROUTINE_QUEUE = std::vector<std::shared_ptr<CRoutine>>;//基于协程的数组
using MULTI_PRIO_QUEUE = std::array<CROUTINE_QUEUE, MAX_PRIO>;//基于优先级的协程数组列表，每一个优先级里存放了协程的数组
using CR_GROUP = std::unordered_map<std::string, MULTI_PRIO_QUEUE>;//基于分组的协程优先级数组列表，用分组名做为key
using LOCK_QUEUE = std::array<base::AtomicRWLock, MAX_PRIO>;//基于优先级的原子读写锁列表，每一个优先级拥有一个原子读写锁
using RQ_LOCK_GROUP = std::unordered_map<std::string, LOCK_QUEUE>;//同样基于分组

using GRP_WQ_MUTEX = std::unordered_map<std::string, MutexWrapper>;//基于分组的Mutex
using GRP_WQ_CV = std::unordered_map<std::string, CvWrapper>;//基于分组的Condition Variable
using NOTIFY_GRP = std::unordered_map<std::string, int>;//基于分组的NOTIFY计数


//下面的变量为static变量，也是在多个线程里共享
alignas(CACHELINE_SIZE) static CR_GROUP cr_group_;
alignas(CACHELINE_SIZE) static RQ_LOCK_GROUP rq_locks_;
alignas(CACHELINE_SIZE) static GRP_WQ_CV cv_wq_;
alignas(CACHELINE_SIZE) static GRP_WQ_MUTEX mtx_wq_;
alignas(CACHELINE_SIZE) static NOTIFY_GRP notify_grp_;

void ClassicContext::InitGroup(const std::string& group_name) {
  multi_pri_rq_ = &cr_group_[group_name];
  lq_ = &rq_locks_[group_name];
  mtx_wrapper_ = &mtx_wq_[group_name];
  cw_ = &cv_wq_[group_name];
  notify_grp_[group_name] = 0;
  current_grp = group_name;
}

```

同时，其调度方法的实现代码如下
```
std::shared_ptr<CRoutine> ClassicContext::NextRoutine() {
  if (cyber_unlikely(stop_.load())) {
    return nullptr;
  }

  //从优先级高的队列开始遍历
  //多个线程共享该队列，设置读锁可以并行获取协程实现多线程并行执行不同的协程任务。
  for (int i = MAX_PRIO - 1; i >= 0; --i) {
    //每一个优先级一个读写锁
    ReadLockGuard<AtomicRWLock> lk(lq_->at(i));
    //从当前有限级高的队列里获取可以运行的协程
    for (auto& cr : multi_pri_rq_->at(i)) {
      if (!cr->Acquire()) {
        continue;
      }

      //如果协程可以调度执行，则返回准备resume
      if (cr->UpdateState() == RoutineState::READY) {
        return cr;
      }
      //否则释放协程，继续查找可以调度的协程
      cr->Release();
    }
  }

  return nullptr;
}
```
##### 经典模式下的调度器`SchedulerClassic`
经典模式的调度策略可以参考配置文件`cyber\conf\example_sched_classic.conf`,
经典模式的调度器的代码为`cyber\scheduler\policy\scheduler_classic.cc`, 其中初始化中调用了`CreateProcessor`函数创建线程，并根据配置设置线程的亲和度和策略。

`cyber\conf\example_sched_classic.conf`中示例如下：下面对一些关键字段做一下说明：

1、group字段：
classic策略可以配置多个group，主要为了实现资源隔离、跨numa问题，比如一个进程产生的所有task在0-31号cpu上执行，内核的调度会将线程在0-31号cpu上切换，跨numa节点会给系统带来额外的开销，这里可以通过group将numa节点进行隔离，一个numa节点下的0-7,16-23号cpu划分到一个group中，另外一个numa节点下的8-15,24-31号cpu划分到另一个group，这样既保证了资源利用，也能避免跨numa节点带来的开销。

2、affinity： 取值为range或者1to1，如第一个group，创建16个线程，在0-7,16-23号cpu上设置亲和性，每个线程都可以在0-7，16-23号核上执行。第二个group中，affinity为1to1，表示16个线程对应8-15,24-31号cpu，每个线程和一个cpu进行亲和性设置，能减少线程在cpu之间切换带来的开销，但是前提是开启的线程数和cpu数必须一致。

3、processor_policy和processor_prio: 这两个一般成对出现，processor_policy指设置线程的调度策略，取值为SCHED_FIFO（实时调度策略，先到先服务）, SCHED_RR（实时调度策略，时间片轮转）, SCHED_OTHER（分时调度策略，为默认策略），对于设置了SCHED_FIFO或者SCHED_RR的线程会更优先的得到cpu执行， 调度模型中设置processor_policy背景：为了保证主链路的任务或者其他一些实时task的优先执行。如果processor_policy设置为SCHED_FIFO/SCHED_RR，processor_prio取值为(1-99)，值越大，表明优先级越高，抢到cpu概率越大。如果processor_policy设置为SCHED_OTHER，processor_prio取值为（-20-19，0为默认值），这里为nice值，nice值不影响分配到cpu的优先级，但是影响分到cpu时间片的大小，如果nice值越小，分到的时间片越多。

4、tasks：这里是对task任务进行配置，name表示task的名字，prio表示任务的优先级，为了提高性能，减小任务队列锁的粒度，调度模型中采用的是多优先级队列，也就是同一优先级的task在同一个队列里面，系统调度时会优先执行优先级高的任务。

```
    groups: [
        {
            name: "group1"
            processor_num: 16
            affinity: "range"
            cpuset: "0-7,16-23"
            processor_policy: "SCHED_OTHER"  # policy: SCHED_OTHER,SCHED_RR,SCHED_FIFO
            processor_prio: 0
            tasks: [
                {
                    name: "E"
                    prio: 0
                }
            ]
        },{
            name: "group2"
            processor_num: 16
            affinity: "1to1"
            cpuset: "8-15,24-31"
            processor_policy: "SCHED_OTHER"
            processor_prio: 0
            tasks: [
                {
                    name: "A"
                    prio: 0
                },{
                    name: "B"
                    prio: 1
                },{
                    name: "C"
                    prio: 2
                },{
                    name: "D"
                    prio: 3
                }
            ]
        }
    ]
```
```
void SchedulerClassic::CreateProcessor() {
  for (auto& group : classic_conf_.groups()) {
    auto& group_name = group.name();
    auto proc_num = group.processor_num();
    if (task_pool_size_ == 0) {
      task_pool_size_ = proc_num;
    }

    auto& affinity = group.affinity();
    auto& processor_policy = group.processor_policy();
    auto processor_prio = group.processor_prio();
    std::vector<int> cpuset;
    ParseCpuset(group.cpuset(), &cpuset);

    for (uint32_t i = 0; i < proc_num; i++) {
      auto ctx = std::make_shared<ClassicContext>(group_name);
      pctxs_.emplace_back(ctx);

      auto proc = std::make_shared<Processor>();
      //线程和上下文绑定，并设置线程的亲和度和优先级
      proc->BindContext(ctx);
      SetSchedAffinity(proc->Thread(), cpuset, affinity, i);
      SetSchedPolicy(proc->Thread(), processor_policy, processor_prio,
                     proc->Tid());
      processors_.emplace_back(proc);
    }
  }
}
```

其分发任务的代码如下,`DispatchTask`函数一般在`Component`组件的初始化里调用，将组件的处理任务封装为协程调用调度器的任务分发函数将其
推送到对应的优先级队列里
```
bool SchedulerClassic::DispatchTask(const std::shared_ptr<CRoutine>& cr) {
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

  {
    //std::unordered_map<uint64_t, std::shared_ptr<CRoutine>> id_cr_;
    //协程的id和协程实例的映射map
    WriteLockGuard<AtomicRWLock> lk(id_cr_lock_);
    if (id_cr_.find(cr->id()) != id_cr_.end()) {
      return false;
    }
    id_cr_[cr->id()] = cr;
  }

  if (cr_confs_.find(cr->name()) != cr_confs_.end()) {
    ClassicTask task = cr_confs_[cr->name()];
    cr->set_priority(task.prio());
    cr->set_group_name(task.group_name());
  } else {
    // croutine that not exist in conf
    cr->set_group_name(classic_conf_.groups(0).name());
  }

  if (cr->priority() >= MAX_PRIO) {
    AWARN << cr->name() << " prio is greater than MAX_PRIO[ << " << MAX_PRIO
          << "].";
    cr->set_priority(MAX_PRIO - 1);
  }

  // Enqueue task.
  {
    WriteLockGuard<AtomicRWLock> lk(
        ClassicContext::rq_locks_[cr->group_name()].at(cr->priority()));
    //获取对应分组的优先级队列下的写锁，开始往对应的优先级的队列里推送协程
    //ClassicContext::cr_group_为静态变量，在不同的线程下面共享，一个ClassicContext为ProcessorContext，对应一个线程
    ClassicContext::cr_group_[cr->group_name()]
        .at(cr->priority())
        .emplace_back(cr);
  }

  ClassicContext::Notify(cr->group_name());
  return true;
}
```

##### FAQ
1、经典模式下，一个分组里的线程共享基于优先级的协程队列，也就是同一个协程可能在不同的线程里进行调度？
是的，下面的协程resume的代码可以看到

```

//static thread_local CRoutine *current_routine_;
//static thread_local char *main_stack_;
//inline char **CRoutine::GetStack() { return &(context_->sp); }

RoutineState CRoutine::Resume() {
  if (cyber_unlikely(force_stop_)) {
    state_ = RoutineState::FINISHED;
    return state_;
  }

  if (cyber_unlikely(state_ != RoutineState::READY)) {
    AERROR << "Invalid Routine State!";
    return state_;
  }

  //将当前协程实例赋给current_routine_
  current_routine_ = this;
  //当前线程里实现协程的上下文切换，将即将resume的协程恢复到主栈并开始执行，主栈的协程的上下文进行保存
  SwapContext(GetMainStack(), GetStack());
  //执行完后current_routine_设为nullptr，等待线程获取下一个协程并进行resume操作
  current_routine_ = nullptr;
  return state_;
}

```
