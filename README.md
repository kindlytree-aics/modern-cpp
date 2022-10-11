## Modern CPP tutorial
本仓库维护关于现代C++相关编程技术的话题，基本的介绍基于现有比较有影响力的开源代码；具体引用的时候在对应的文档里会有说明。欢迎大家的反馈，我们会不断优化更新相关内容。

### 开发环境准备：
- IDE：https://code.visualstudio.com/
- 编译器下载及使用
    - 下载链接： https://github.com/niXman/mingw-builds-binaries/releases
    - 选择版本： x86_64-12.2.0-release-posix-seh-rt_v10-rev0.7z
        - posix：支持std:thread
        - seh和sjlj: 是两个不同的异常处理系统,暂时不用太关注，我们选择seh
- 配置方法：
    - 设置环境变量PATH中添加编译器解压的目录，注意要到bin这一层，如`D:\software\gcc\mingw64\bin`
    - vscode中安装必要的插件，如c/c++插件
    - 具体配置请参考视频演示：https://www.bilibili.com/video/BV1Ee411K7Hq

### 内容列表:

#### 新特性
- [右值引用]()
- [std::move]()
- [lamda表达式]()

#### 多线程(并发)编程
- [线程的等待策略](./docs/wait_stategy.md)
- [线程安全队列](./docs/bounded_queue.md)
- [有界队列及其无锁实现](./docs/bounded_queue.md)
- [原子读写锁的实现](./docs/atomic_rw_lock.md)
- [协程](./docs/coroutine.md)

### 更多内容
- 抖音号：kindlytree_aics
- 知乎主页：https://www.zhihu.com/people/kindlytree

### Reference
- https://en.cppreference.com/
- www.cplusplus.com/doc/tutorial
- https://web.stanford.edu/class/cs106b/
- https://github.com/rigtorp/awesome-modern-cpp
- https://github.com/changkun/modern-cpp-tutorial
- https://github.com/CnTransGroup/EffectiveModernCppChinese
