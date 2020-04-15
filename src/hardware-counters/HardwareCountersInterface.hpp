/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2020 Barcelona Supercomputing Center (BSC)
*/

#ifndef HARDWARE_COUNTERS_INTERFACE_HPP
#define HARDWARE_COUNTERS_INTERFACE_HPP

#include <string>

#include "SupportedHardwareCounters.hpp"


class Task;

class HardwareCountersInterface {

public:

	virtual ~HardwareCountersInterface()
	{
	}

	virtual bool isSupported(HWCounters::counters_t counterType) const = 0;

	virtual void threadInitialized() = 0;

	virtual void threadShutdown() = 0;

	virtual void taskCreated(Task *task, bool enabled) = 0;

	virtual void taskReinitialized(Task *task) = 0;

	virtual void taskStarted(Task *task) = 0;

	virtual void taskStopped(Task *task) = 0;

	virtual void taskFinished(Task *task) = 0;

	virtual size_t getTaskHardwareCountersSize() const = 0;

};

#endif // HARDWARE_COUNTERS_INTERFACE_HPP