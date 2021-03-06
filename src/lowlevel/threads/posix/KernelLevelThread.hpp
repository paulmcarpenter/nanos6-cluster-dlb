/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2015-2020 Barcelona Supercomputing Center (BSC)
*/

#ifndef POSIX_KERNEL_LEVEL_THREAD_HPP
#define POSIX_KERNEL_LEVEL_THREAD_HPP

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cassert>

#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <MemoryAllocator.hpp>

#include "executors/threads/CPU.hpp"
#include "lowlevel/ConditionVariable.hpp"
#include "lowlevel/FatalErrorHandler.hpp"


class KernelLevelThread {
protected:
	//! The underlying pthread
	pthread_t _pthread;
	pid_t _tid;

	//! This condition variable is used for suspending and resuming the thread
	ConditionVariable _suspensionConditionVariable;

	//! stack info to appropriate deallocate it
	size_t _stackSize;
	void *_stackPtr;

	//! Thread Local Storage variable to point back to the KernelLevelThread that is running the code
	static __thread KernelLevelThread *_currentKernelLevelThread;


	inline void exit()
	{
		pthread_exit(nullptr);
	}

	inline void setCurrentKernelLevelThread()
	{
		_currentKernelLevelThread = this;
	}

public:
	KernelLevelThread()
		: _stackSize(0), _stackPtr(nullptr)
	{
	}

	void *getStackAndSize(size_t &size)
	{
		size = _stackSize;
		return _stackPtr;
	}

	virtual ~KernelLevelThread()
	{
		if (_stackSize > 0) {
			assert(_stackPtr != nullptr);
			MemoryAllocator::free(_stackPtr, _stackSize);
		}
	}

	// WARNING: This should be only called by the thread initialization code
	inline void setTid(pid_t tid)
	{
		_tid = tid;
	}

	inline pid_t getTid() const
	{
		return _tid;
	}

	inline void bind(CPU *cpu)
	{
		assert(cpu != nullptr);
		int rc = sched_setaffinity(_tid, CPU_ALLOC_SIZE(cpu->getSystemCPUId()+1), cpu->getCpuMask());
		FatalErrorHandler::handle(rc,
			" when changing affinity of pthread with thread id ", _tid,
			" to CPU ", cpu->getSystemCPUId()
		);
	}

	//! \brief Suspend the thread
	inline void suspend()
	{
		_suspensionConditionVariable.wait();
	}

	//! \brief Resume the thread
	inline void resume()
	{
		_suspensionConditionVariable.signal();
	}

	//! \brief Wait for the thread to finish and join it
	inline void join()
	{
		int rc = pthread_join(_pthread, nullptr);
		FatalErrorHandler::handle(rc, " during shutdown when joining pthread ", _pthread);
	}

	//! \brief check if the thread will resume immediately when calling to suspend
	inline bool willResumeImmediately()
	{
		return _suspensionConditionVariable.isPresignaled();
	}

	//! \brief clear the pending resumption mark
	inline void abortResumption()
	{
		_suspensionConditionVariable.clearPresignal();
	}

	//! \brief code that the thread executes
	virtual void body() = 0;

	static inline KernelLevelThread *getCurrentKernelLevelThread()
	{
		return _currentKernelLevelThread;
	}


	static void *kernel_level_thread_body_wrapper(void *parameter)
	{
		KernelLevelThread *thread = static_cast<KernelLevelThread *> (parameter);

		assert(thread != nullptr);
		thread->setTid(syscall(SYS_gettid));

		KernelLevelThread::_currentKernelLevelThread = thread;

		thread->body();

		return nullptr;
	}

	void start(pthread_attr_t *pthreadAttr)
	{
		void *stackptr;
		size_t stacksize;
		int rc;

		if (pthreadAttr != nullptr) {
			rc = pthread_attr_getstacksize(pthreadAttr, &stacksize);
			FatalErrorHandler::handle(rc, " when getting pthread's stacksize");

			stackptr = MemoryAllocator::alloc(stacksize);
			FatalErrorHandler::failIf(stackptr == nullptr, " when allocating pthread stack");
			_stackSize = stacksize;
			_stackPtr = stackptr;

			rc = pthread_attr_setstack(pthreadAttr, stackptr, stacksize);
			FatalErrorHandler::handle(rc, " when setting pthread's stack");
		}

		rc = pthread_create(&_pthread, pthreadAttr, &KernelLevelThread::kernel_level_thread_body_wrapper, this);
		if (rc == EAGAIN) {
			FatalErrorHandler::fail(
				" Insufficient resources when creating a pthread. This may happen due to:\n",
				"  (1) Having reached the system-imposed limit of threads\n",
				"  (2) The stack size limit is too large, try decreasing it with 'ulimit'"
			);
		} else {
			FatalErrorHandler::handle(rc, " when creating a pthread");
		}
	}
};

#endif // POSIX_KERNEL_LEVEL_THREAD_HPP
