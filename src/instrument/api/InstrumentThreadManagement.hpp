/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.
	
	Copyright (C) 2015-2017 Barcelona Supercomputing Center (BSC)
*/

#ifndef INSTRUMENT_THREAD_MANAGEMENT_HPP
#define INSTRUMENT_THREAD_MANAGEMENT_HPP

#include <string>

#include <InstrumentComputePlaceId.hpp>
#include <InstrumentExternalThreadId.hpp>
#include <InstrumentThreadId.hpp>


namespace Instrument {
	//! This function is called when the runtime creates a new thread and
	//! must return an instrumentation-specific thread identifier that will
	//! be used to identify it throughout the rest of the instrumentation API.
	void createdThread(/* OUT */ thread_id_t &threadId, compute_place_id_t const &computePlaceId);
	
	//! This function is called when the runtime creates a new non-worker thread and
	//! must return an instrumentation-specific thread identifier that will
	//! be used to identify it throughout the rest of the instrumentation API.
	template<typename... TS>
	void createdExternalThread(/* OUT */ external_thread_id_t &threadId, TS... nameComponents);
	
	void threadWillSuspend(thread_id_t threadId, compute_place_id_t cpu);
	void threadHasResumed(thread_id_t threadId, compute_place_id_t cpu);
	void threadWillShutdown();
	
	enum busy_wait_reason_t {
		scheduling_polling_slot_busy_wait_reason = 1
	};
	
	void threadEnterBusyWait(busy_wait_reason_t reason);
	void threadExitBusyWait();
}


#endif // INSTRUMENT_THREAD_MANAGEMENT_HPP
