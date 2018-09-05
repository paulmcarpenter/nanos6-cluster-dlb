/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.
	
	Copyright (C) 2015-2018 Barcelona Supercomputing Center (BSC)
*/

#ifndef __VIRTUAL_MEMORY_MANAGEMENT_HPP__
#define __VIRTUAL_MEMORY_MANAGEMENT_HPP__

#include "memory/vmm/VirtualMemoryArea.hpp"

#include <vector>

class VirtualMemoryManagement {
private:
	//! initial allocation from OS
	static void *_address;
	static size_t _size;
	
	//! System's page size
	static size_t _pageSize;
	
	//! addresses for local NUMA allocations
	static std::vector<VirtualMemoryArea *> _localNUMAVMA;
	
	//! addresses for generic allocations
	static VirtualMemoryArea *_genericVMA;
	
	//! Setting up the memory layout
	static void setupMemoryLayout(void *address, size_t distribSize, size_t localSize);
	
	//! private constructor, this is a singleton.
	VirtualMemoryManagement()
	{
	}
public:
	static void initialize();
	static void shutdown();
	
	/** allocate a block of generic addresses.
	 *
	 * This region is meant to be used for allocations that can be mapped
	 * to various memory nodes (cluster or NUMA) based on a policy. So this
	 * is the pool for distributed allocations or other generic allocations.
	 */
	static inline void *allocDistrib(size_t size)
	{
		return _genericVMA->allocBlock(size);
	}
	
	/** allocate a block of local addresses on a NUMA node.
	 *
	 * \param size the size to allocate
	 * \param NUMAId is the the id of the NUMA node to allocate
	 */
	static inline void *allocLocalNUMA(size_t size, size_t NUMAId)
	{
		VirtualMemoryArea *vma = _localNUMAVMA.at(NUMAId);
		return vma->allocBlock(size);
	}
};


#endif /* __VIRTUAL_MEMORY_MANAGEMENT_HPP__ */
