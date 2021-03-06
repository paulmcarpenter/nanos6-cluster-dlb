/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2019 Barcelona Supercomputing Center (BSC)
*/

#ifndef __OBJECT_CACHE_HPP__
#define __OBJECT_CACHE_HPP__

#include "lowlevel/SpinLock.hpp"
#include "hardware/HardwareInfo.hpp"
#include "NUMAObjectCache.hpp"
#include "CPUObjectCache.hpp"
#include "executors/threads/CPU.hpp"
#include "executors/threads/WorkerThread.hpp"

template<typename T>
class ObjectCache {

	/** An object cache is built in two layers, one CPU and one NUMA layer.
	 *
	 * Allocations will happen through the local CPU cache, or the external
	 * object cache. The CPUObjectCache will invoke the NUMAObjectCache to
	 * get more objects if it runs out of objects.
	 *
	 * Deallocations will happen to the CPUObjectCache of the current CPU.
	 * If the object does not belong to that CPUObjectCache (it belongs in
	 * a different NUMA node) the object will be returned to the
	 * NUMAObjectCache in order to be used from the CPUObjectCache of the
	 * respective NUMA node. */
	NUMAObjectCache<T> *_NUMACache;
	std::vector<CPUObjectCache<T> *> _CPUCaches;
	CPUObjectCache<T> *_externalObjectCache;
	SpinLock _externalLock;

public:
	ObjectCache()
	{
		const size_t numaNodeCount =
			HardwareInfo::getMemoryPlaceCount(nanos6_device_t::nanos6_host_device);
		const size_t cpuCount = CPUManager::getTotalCPUs();

		std::vector<CPU *> const &cpus = CPUManager::getCPUListReference();
		assert(cpus.size() == cpuCount);

		_NUMACache = new NUMAObjectCache<T>(numaNodeCount);
		_CPUCaches.resize(cpuCount);
		for (size_t i = 0; i < cpuCount; ++i) {
			CPU *cpu = cpus[i];
			_CPUCaches[i] = new CPUObjectCache<T>(_NUMACache, cpu->getNumaNodeId(), numaNodeCount);
		}
		_externalObjectCache = new CPUObjectCache<T>(_NUMACache, /* NUMA Id */ 0, numaNodeCount);
	}

	~ObjectCache()
	{
		delete _NUMACache;
		for (auto it : _CPUCaches) {
			delete it;
		}
		delete _externalObjectCache;
	}

	template<typename... TS>
	inline T *newObject(TS &&... args)
	{
		WorkerThread *thread = WorkerThread::getCurrentWorkerThread();
		CPU *cpu = (thread != nullptr ? thread->getComputePlace() : nullptr);

		T *addr;
		if (cpu == nullptr) {
			std::lock_guard<SpinLock> guard(_externalLock);
			addr =  _externalObjectCache->newObject(std::forward<TS>(args)...);
		} else {
			const size_t cpuId = cpu->getIndex();
			assert(cpuId < _CPUCaches.size());
			addr = _CPUCaches[cpuId]->newObject(std::forward<TS>(args)...);
		}
		return addr;
	}

	inline void deleteObject(T *ptr)
	{
		WorkerThread *thread = WorkerThread::getCurrentWorkerThread();
		CPU *cpu = (thread != nullptr ? thread->getComputePlace() : nullptr);

		if (cpu == nullptr) {
			std::lock_guard<SpinLock> guard(_externalLock);
			_externalObjectCache->deleteObject(ptr);
		} else {
			size_t cpuId = cpu->getIndex();
			_CPUCaches[cpuId]->deleteObject(ptr);
		}
	}

	// Function to get the total number of allocated object if this type.
	// This is an estimated value because we don't use a lock in any moment.
	// This is a debug function.
	size_t getNumObject() const
	{
		size_t ret = 0;

		for (CPUObjectCache<T> *it : _CPUCaches) {
			ret += it->getCounter();
		}

		return ret;
	}

};

#endif /* __OBJECT_CACHE_HPP__ */
