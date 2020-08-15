#include <queue>
#include <mutex>
#include <optional>

template <class Predicate>
void waitUntil(Predicate &&predicate) {
	u32 tryCount = 0;
	while (!predicate()) {
		spinIteration();
		if (tryCount >= 64)
			switchThread();
		if (tryCount >= 4096)
			sleepMilliseconds(1);
		++tryCount;
	}
}
#if 0
struct Mutex : std::mutex {
	Waiter *waiter;
};

Mutex makeMutex(Waiter *waiter = defaultWaiter) { return {}; }

void lock(Mutex *m) {
	m->lock();
}
void unlock(Mutex *m) {
	m->unlock();
}
#else
struct Mutex {
	bool volatile inUse;
};

void lock(Mutex *m) {
	waitUntil([&] {
		return !lockSetIfEquals(&m->inUse, true, false);
	});
}
void unlock(Mutex *m) {
	m->inUse = false;
}
#endif
template <class T>
struct MutexQueue {
	Queue<T> base;
	Mutex mutex;
	
	void push(T &&value) {
		lock(&mutex);
		DEFER { unlock(&mutex); };
		base.push(std::move(value));
	}
	void push(T const &value) {
		lock(&mutex);
		DEFER { unlock(&mutex); };
		base.push(value);
	}
	std::optional<T> try_pop() {
		std::optional<T> result;
		lock(&mutex);
		DEFER { unlock(&mutex); };
		if (base.size()) {
			result.emplace(std::move(base.front()));
			base.pop();
		}
		return result;
	}
	T pop() {
		while (1) {
			if (auto t = try_pop()) {
				return t.value();
			}
		}
	}
};

struct WorkQueue;

struct ThreadWork {
	WorkQueue *queue;
	void (*fn)(void *param);
	void *param;
};

struct ThreadPool {
	Allocator allocator = osAllocator;
	HANDLE *threads = 0;
	u32 threadCount = 0;
	u32 volatile initializedThreadCount = 0;
	u32 volatile deadThreadCount = 0;
	bool volatile doingWork = false;
	MutexQueue<ThreadWork> allWork;
};
bool tryDoWork(ThreadPool *pool);

namespace Detail {
template <class Tuple, umm... indices>
static void invoke(void *rawVals) noexcept {
	Tuple *fnVals((Tuple *)(rawVals));
	Tuple &tup = *fnVals;
	std::invoke(std::move(std::get<indices>(tup))...);
}

template <class Tuple, umm... indices>
static constexpr auto getInvoke(std::index_sequence<indices...>) noexcept {
	return &invoke<Tuple, indices...>;
}
} // namespace Detail

struct WorkQueue {
	u32 volatile workToDo = 0;
	ThreadPool *pool = 0;
	void push(void (*fn)(void *param), void *param) {
		if (pool->threadCount) {
			ThreadWork work;
			work.fn = fn;
			work.param = param;
			work.queue = this;
			pool->allWork.push(work);
		} else {
			fn(param);
		}
	}
	template <class Fn, class... Args>
	void push(Fn &&fn, Args &&... args) {
		if (pool->threadCount) {
			using Tuple = std::tuple<std::decay_t<Fn>, std::decay_t<Args>...>;
			auto fnParams = allocate<Tuple>(pool->allocator);
			new(fnParams) Tuple(std::forward<Fn>(fn), std::forward<Args>(args)...);
			constexpr auto invokerProc = Detail::getInvoke<Tuple>(std::make_index_sequence<1 + sizeof...(Args)>{});
			lockIncrement(workToDo);
			push((void (*)(void *))invokerProc, (void *)fnParams);
		} else {
			fn(std::forward<Args>(args)...);
		}
	}
	void completeAllWork() {
		if (pool->threadCount) {
			waitUntil([&] {
				tryDoWork(pool);
				return workToDo == 0;
			});
		}
	}

};

WorkQueue makeWorkQueue(ThreadPool *pool) {
	WorkQueue result = {};
	result.pool = pool;
	return result;
}

void doWork(ThreadWork work) {
	work.fn(work.param);
	deallocate(work.queue->pool->allocator, work.param);
	lockDecrement(work.queue->workToDo);
}
bool tryDoWork(ThreadPool *pool) {
	if (auto popped = pool->allWork.try_pop()) {
		doWork(*popped);
		return true;
	}
	return false;
}
bool doWork(ThreadPool *pool) {
	ThreadWork work = {};
	waitUntil([&] {
		if (!pool->doingWork) {
			return true;
		}
		if (auto popped = pool->allWork.try_pop()) {
			work = *popped;
			return true;
		}
		return false;
	});

	if (work.fn) {
		doWork(work);
		return true;
	} else {
		return false;
	}
}

void defaultThreadPoolProc(ThreadPool *pool) {
	lockIncrement(pool->initializedThreadCount);
	waitUntil([&] { return pool->doingWork; });
	while (1) {
		if (!doWork(pool))
			break;
	}
	lockIncrement(pool->deadThreadCount);
}
template <class ThreadProc = decltype(defaultThreadPoolProc)>
bool initThreadPool(ThreadPool *pool, u32 threadCount, ThreadProc &&threadProc = defaultThreadPoolProc) {
	TIMED_FUNCTION;
	pool->initializedThreadCount = 0;
	pool->deadThreadCount = 0;
	pool->doingWork = false;
	pool->threadCount = threadCount;
	if (threadCount) {
		pool->threads = allocate<HANDLE>(pool->allocator, threadCount);
		
		struct StartParams {
			ThreadPool *pool;
			ThreadProc *proc;
		};
		StartParams params;
		params.pool = pool;
		params.proc = std::addressof(threadProc);
		
		auto startProc = [](void *param) -> DWORD {
			StartParams *info = (StartParams *)param;
			(*info->proc)(info->pool);
			return 0;
		};

		for (u32 i = 0; i < threadCount; ++i) {
			pool->threads[i] = CreateThread(0, 0, startProc, &params, 0, 0);
			if (!pool->threads[i]) {
				for (u32 j = 0; j < i; ++j) {
					CloseHandle(pool->threads[j]);
				}
				return false;
			}
		}

		waitUntil([&] {
			return pool->initializedThreadCount == pool->threadCount;
		});
		pool->doingWork = true;
	} else {
		pool->threads = 0;
	}

	return true;
}
void deinitThreadPool(ThreadPool *pool, bool waitForThreads = true) {
	pool->doingWork = false;
	if (waitForThreads) {
		waitUntil([&] { return pool->deadThreadCount == pool->threadCount; });
	}
}