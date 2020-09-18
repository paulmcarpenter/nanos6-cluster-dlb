/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.

	Copyright (C) 2020 Barcelona Supercomputing Center (BSC)
*/


#include <cassert>
#include <errno.h>
#include <iostream>
#include <string>

#include "CTFAPI.hpp"
#include "CTFTrace.hpp"
#include "CTFTypes.hpp"
#include "CTFEvent.hpp"

#include <lowlevel/FatalErrorHandler.hpp>

extern CTFAPI::CTFEvent *__eventCTFFlush;

uint64_t CTFAPI::getTimestamp()
{
	uint64_t timestamp;
	struct timespec tp;
	const uint64_t ns = 1000000000ULL;

	// TODO using clock_gettime requires to add -lrt for old glibcs. We
	// might get rid of this by using the chrono c++ library, but I'm not
	// confident on which syscall and clock this translates. Should we move
	// to c++ here? If so, remove the -lrt

	if (clock_gettime(CLOCK_MONOTONIC, &tp)) {
		FatalErrorHandler::failIf(true, std::string("Instrumentation: ctf: clock_gettime syscall: ") + strerror(errno));
	}
	timestamp = tp.tv_sec * ns + tp.tv_nsec;

	return timestamp;
}

uint64_t CTFAPI::getRelativeTimestamp()
{
	uint64_t timestamp;
	CTFTrace &trace = CTFTrace::getInstance();

	timestamp = getTimestamp();
	timestamp -= trace.getAbsoluteStartTimestamp();

	return timestamp;
}

void CTFAPI::mk_event_header(char **buf, uint64_t timestamp, uint8_t id)
{
	struct event_header *pk;

	pk = (struct event_header *) *buf;
	*pk = (struct event_header) {
		.id = id,
		.timestamp = timestamp
	};

	*buf += sizeof(struct event_header);
}

void CTFAPI::writeFlushingTracepoint(CTFStream *stream,
				      uint64_t tsBefore, uint64_t tsAfter)
{
	uint64_t timestamp = getRelativeTimestamp();

	__tp_lock(stream, __eventCTFFlush, timestamp, tsBefore, tsAfter);
}

void CTFAPI::flushAll(CTFStream *stream,
			 uint64_t *tsBefore, uint64_t *tsAfter)
{
	*tsBefore = getRelativeTimestamp();
	stream->flushAll();
	*tsAfter = getRelativeTimestamp();
}

void CTFAPI::flushSubBuffers(CTFStream *stream,
			 uint64_t *tsBefore, uint64_t *tsAfter)
{
	*tsBefore = getRelativeTimestamp();
	stream->flushFilledSubBuffers();
	*tsAfter = getRelativeTimestamp();
}

void CTFAPI::flushCurrentVirtualCPUBufferIfNeeded()
{
	uint64_t tsBefore, tsAfter;
	CTFStream *stream = Instrument::getCTFCPULocalData()->userStream;
	assert(stream != nullptr);

	stream->lock();
	if (stream->checkIfNeedsFlush()) {
		flushSubBuffers(stream, &tsBefore, &tsAfter);
		writeFlushingTracepoint(stream, tsBefore, tsAfter);
	}
	stream->unlock();
}