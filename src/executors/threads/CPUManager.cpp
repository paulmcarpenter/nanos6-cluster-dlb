/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.
	
	Copyright (C) 2015-2017 Barcelona Supercomputing Center (BSC)
*/

#include <boost/dynamic_bitset.hpp>
#include <cassert>
#include <sched.h>
#include <sstream>

#include "CPU.hpp"
#include "CPUManager.hpp"
#include "ThreadManager.hpp"
#include "WorkerThread.hpp"
#include "hardware/HardwareInfo.hpp"
#include "system/RuntimeInfo.hpp"


std::vector<CPU *> CPUManager::_cpus;
size_t CPUManager::_totalCPUs;
std::atomic<bool> CPUManager::_finishedCPUInitialization;
SpinLock CPUManager::_idleCPUsLock;
boost::dynamic_bitset<> CPUManager::_idleCPUs;
std::vector<boost::dynamic_bitset<>> CPUManager::_NUMANodeMask;
std::vector<size_t> CPUManager::_systemToVirtualCPUId;


namespace cpumanager_internals {
	static inline std::string maskToRegionList(boost::dynamic_bitset<> const &mask,
			std::vector<CPU *> cpus)
	{
		std::ostringstream oss;
		
		size_t size = cpus.size();
		int start = 0;
		int end = -1;
		bool first = true;
		
		for (size_t virtualCPUId = 0; virtualCPUId < size; ++virtualCPUId) {
			if ((virtualCPUId < size) && mask[virtualCPUId]) {
				CPU *cpu = cpus[virtualCPUId];
				size_t systemCPUId = cpu->getSystemCPUId();
				if (end >= start) {
					// Valid region: extend
					end = systemCPUId;
				} else {
					// Invalid region: start
					start = systemCPUId;
					end = systemCPUId;
				}
			} else {
				if (end >= start) {
					// Valid region: emit and invalidate
					if (first) {
						first = false;
					} else {
						oss << ",";
					}
					if (end == start) {
						oss << start;
					} else {
						oss << start << "-" << end;
					}
					end = -1;
				} else {
					// Invalid region: do nothing
				}
			}
		}
		
		return oss.str();
	}
	
	
	static inline std::string maskToRegionList(cpu_set_t const &mask, size_t size)
	{
		std::ostringstream oss;
		
		int start = 0;
		int end = -1;
		bool first = true;
		for (size_t i = 0; i < size+1; i++) {
			if ((i < size) && CPU_ISSET(i, &mask)) {
				if (end >= start) {
					// Valid region: extend
					end = i;
				} else {
					// Invalid region: start
					start = i;
					end = i;
				}
			} else {
				if (end >= start) {
					// Valid region: emit and invalidate
					if (first) {
						first = false;
					} else {
						oss << ",";
					}
					if (end == start) {
						oss << start;
					} else {
						oss << start << "-" << end;
					}
					end = -1;
				} else {
					// Invalid region: do nothing
				}
			}
		}
		
		return oss.str();
	}
}


void CPUManager::preinitialize()
{
	_finishedCPUInitialization = false;
	_totalCPUs = 0;
	
	cpu_set_t processCPUMask;
	int rc = sched_getaffinity(0, sizeof(cpu_set_t), &processCPUMask);
	FatalErrorHandler::handle(rc, " when retrieving the affinity of the process");
	
	// Get NUMA nodes
	_NUMANodeMask.resize(HardwareInfo::getMemoryPlaceCount(nanos6_device_t::nanos6_host_device));
	
	// Get CPU objects that can run a thread
	std::vector<ComputePlace *> const &cpus = ((HostInfo *) HardwareInfo::getDeviceInfo(nanos6_device_t::nanos6_host_device))->getComputePlaces();
	
	size_t maxSystemCPUId = 0;
	for (auto const *computePlace : cpus) {
		CPU const *cpu = (CPU const *) computePlace;
		
		if (cpu->getSystemCPUId() > maxSystemCPUId) {
			maxSystemCPUId = cpu->getSystemCPUId();
		}
	}
	
	int cpuMaskSize = CPU_COUNT(&processCPUMask);
	_cpus.resize(cpuMaskSize);;
	_systemToVirtualCPUId.resize(maxSystemCPUId+1);
	
	for (size_t i = 0; i < _NUMANodeMask.size(); ++i) {
		_NUMANodeMask[i].resize(cpuMaskSize);
	}
	
	for (size_t i = 0; i < cpus.size(); ++i) {
		CPU *cpu = (CPU *)cpus[i];
		
		size_t virtualCPUId;
		if (CPU_ISSET(cpu->getSystemCPUId(), &processCPUMask)) {
			virtualCPUId = _totalCPUs;
			cpu->setIndex(virtualCPUId);
			_cpus[virtualCPUId] = cpu;
			++_totalCPUs;
			_NUMANodeMask[cpu->getNumaNodeId()][virtualCPUId] = true;
		} else {
			virtualCPUId = (size_t) ~0UL;
			cpu->setIndex(virtualCPUId);
		}
		_systemToVirtualCPUId[cpu->getSystemCPUId()] = cpu->getIndex();
	}
	
	RuntimeInfo::addEntry("initial_cpu_list", "Initial CPU List", cpumanager_internals::maskToRegionList(processCPUMask, cpus.size()));
	for (size_t i = 0; i < _NUMANodeMask.size(); ++i) {
		std::ostringstream oss, oss2;
		
		oss << "numa_node_" << i << "_cpu_list";
		oss2 << "NUMA Node " << i << " CPU List";
		std::string cpuRegionList = cpumanager_internals::maskToRegionList(_NUMANodeMask[i], _cpus);
		
		RuntimeInfo::addEntry(oss.str(), oss2.str(), cpuRegionList);
	}
	
	// Set all CPUs as not idle
	_idleCPUs.resize(_cpus.size());
	_idleCPUs.reset();
}


void CPUManager::initialize()
{
	for (size_t virtualCPUId = 0; virtualCPUId < _cpus.size(); ++virtualCPUId) {
		if (_cpus[virtualCPUId] != nullptr) {
			CPU *cpu = _cpus[virtualCPUId];
			assert(cpu != nullptr);
			
			bool worked = cpu->initializeIfNeeded();
			if (worked) {
				WorkerThread *initialThread = ThreadManager::createWorkerThread(cpu);
				initialThread->resume(cpu, true);
			} else {
				// Already initialized?
			}
		}
	}
	
	_finishedCPUInitialization = true;
}
