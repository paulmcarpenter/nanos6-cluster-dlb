#if HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "InstrumentProfile.hpp"

#include "lowlevel/FatalErrorHandler.hpp"

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>

#if HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#if HAVE_LIBUNWIND_H
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

// Workaround for missing definition
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif


#define LOWEST_VALID_ADDRESS 1024UL


Instrument::Profile Instrument::Profile::_singleton;
__thread Instrument::Profile::PerThread Instrument::Profile::_perThread;
bool Instrument::Profile::_enabled;


void Instrument::Profile::sigprofHandler(__attribute__((unused)) int signal, __attribute__((unused)) siginfo_t *sigInfo, __attribute__((unused))  void *signalContext)
{
	if (__builtin_expect(!_enabled, 0)) {
		struct itimerspec it;
		it.it_interval = {0, 0};
		it.it_value = {0, 0};
		
		// Disarm the timer
		timer_settime(_perThread._profilingTimer, 0, &it, 0);
		
		return;
	}
	
	long depth = _singleton._profilingBacktraceDepth;
	long bufferSize = _singleton._profilingBufferSize;
	
	if (_perThread._nextBufferPosition + depth > (bufferSize + 2)) {
		int rc = posix_memalign((void **) &_perThread._currentBuffer, 128, sizeof(address_t) * bufferSize);
		FatalErrorHandler::handle(rc, " allocating a buffer of ", sizeof(address_t) * bufferSize, " bytes for profiling");
		
		_perThread._nextBufferPosition = 0;
		
		_singleton._bufferListSpinLock.lock();
		_singleton._bufferList.push_back(_perThread._currentBuffer);
		_singleton._bufferListSpinLock.unlock();
	}
	
#if HAVE_LIBUNWIND_H
	unw_context_t context;
	unw_getcontext(&context);
	unw_cursor_t cursor;
	unw_init_local(&cursor, &context);
	
	int currentFrame = 0;
	
	bool haveAFrame = (unw_step(&cursor) > 0); // Skip this function
	haveAFrame = (unw_step(&cursor) > 0); // Skip the signal frame
	
	while (haveAFrame && (currentFrame < depth)) {
		if (unw_get_reg(&cursor, UNW_REG_IP, (unw_word_t *) &_perThread._currentBuffer[_perThread._nextBufferPosition]) == 0) {
			if (_perThread._currentBuffer[_perThread._nextBufferPosition] >= (address_t) LOWEST_VALID_ADDRESS) {
				_perThread._nextBufferPosition++;
				currentFrame++;
			}
			haveAFrame = (unw_step(&cursor) > 0);
		} else {
			haveAFrame = false;
		}
	}
#elif defined(HAVE_EXECINFO_H) && defined(HAVE_BACKTRACE)
	// Get the number of backtrace addresses + one for this function + one for the signal frame
	address_t addresses[depth+2];
	int frames = backtrace((void **) &_perThread._currentBuffer[_perThread._nextBufferPosition], depth+2);
	
	// Skip this function and the signal frame
	for (int frame=2; frame < frames; frame++) {
		if (addresses[frame]  >= (address_t) LOWEST_VALID_ADDRESS) {
			_perThread._currentBuffer[_perThread._nextBufferPosition] = addresses[frame];
			_perThread._nextBufferPosition++;
		}
	}
#else
	#warning Profiling is not supported in this platform
#endif
	
	// End of backtrace mark
	_perThread._currentBuffer[_perThread._nextBufferPosition] = 0;
	_perThread._nextBufferPosition++;
	
	// We keep always an end mark in the buffer and add it to the list of buffers.
	// This way we do not need to perform any kind of cleanup for the threads
	_perThread._currentBuffer[_perThread._nextBufferPosition] = 0; // The end mark
}


void Instrument::Profile::doCreatedThread(__attribute__((unused)) WorkerThread *thread)
{
	#if !defined(HAVE_BACKTRACE) && !defined(HAVE_LIBUNWIND)
		std::cerr << "Warning: profiling currently not supported in this platform." << std::endl;
		return;
	#endif
	
	struct sigevent se;
	se.sigev_notify = SIGEV_THREAD_ID;
	se.sigev_signo = SIGPROF;
	se.sigev_value.sival_int = 1;
	se.sigev_notify_thread_id = syscall(SYS_gettid);
	
	int rc = posix_memalign((void **) &_perThread._currentBuffer, 128, sizeof(address_t) * _profilingBufferSize);
	FatalErrorHandler::handle(rc, " allocating a buffer of ", sizeof(address_t) * _profilingBufferSize, " bytes for profiling");
	
	_perThread._nextBufferPosition = 0;
	
	// We keep always an end mark in the buffer and add it to the list of buffers.
	// This way we do not need to perform any kind of cleanup for the threads
	
	_perThread._currentBuffer[_perThread._nextBufferPosition] = 0; // End of backtrace
	_perThread._currentBuffer[_perThread._nextBufferPosition+1] = 0; // End of buffer
	
	_bufferListSpinLock.lock();
	_bufferList.push_back(_perThread._currentBuffer);
	_bufferListSpinLock.unlock();
	
	// We call the signal handler once since the first call to backtrace allocates memory.
	// If the signal is delivered within a memory allocation, the thread can deadlock.
	sigprofHandler(0, 0, 0);
	
	// Remove the sample
	_perThread._nextBufferPosition = 0;
	_perThread._currentBuffer[0] = 0; // End of backtrace
	_perThread._currentBuffer[1] = 0; // End of buffer
	
	// Profiling actually starts after the follwoing lines
	rc = timer_create(CLOCK_THREAD_CPUTIME_ID, &se, &_perThread._profilingTimer);
	FatalErrorHandler::handle(rc, " creating a timer for profiling");
	struct itimerspec it = {
		.it_interval = { .tv_sec = 0, .tv_nsec = _profilingNSResolution },
		.it_value = { .tv_sec = 0, .tv_nsec = _profilingNSResolution }
	};
	
	rc = timer_settime(_perThread._profilingTimer, 0, &it, 0);
	FatalErrorHandler::handle(rc, " arming the timer for profiling");
}


void Instrument::Profile::doInit()
{
	#if !defined(HAVE_BACKTRACE) && !defined(HAVE_LIBUNWIND)
		std::cerr << "Warning: profiling currently not supported in this platform." << std::endl;
		return;
	#endif
	
	struct sigaction sa;
	sa.sa_sigaction = (void (*)(int, siginfo_t *, void *)) sigprofHandler;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGPROF);
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	
	int rc = sigaction(SIGPROF, &sa, 0);
	FatalErrorHandler::handle(rc, " programming the SIGPROF signal handler");
	
	_enabled = true;
	std::atomic_thread_fence(std::memory_order_seq_cst);
}


inline Instrument::Profile::AddrInfo const &Instrument::Profile::resolveAddress(Instrument::Profile::address_t address)
{
	{
		auto it = _addr2Cache.find(address);
		if (it != _addr2Cache.end()) {
			return it->second;
		}
	}
	
	auto it = _executableMemoryMap.upper_bound(address);
	if (it == _executableMemoryMap.begin()) {
		// The address cannot be resolved
		return _unknownAddrInfo;
	}
	it--;
	
	MemoryMapSegment const &memoryMapSegment = it->second;
	
	if (memoryMapSegment._filename.empty()) {
		return _unknownAddrInfo;
	}
	
	
	AddrInfo addrInfo;
	
	size_t relativeAddress = (size_t)address - (size_t)it->first;
	
	std::ostringstream addr2lineCommandLine;
	addr2lineCommandLine << "addr2line -i -f -C -e " << memoryMapSegment._filename << " " << std::hex << relativeAddress;
	
	FILE *addr2lineOutput = popen(addr2lineCommandLine.str().c_str(), "r");
	if (addr2lineOutput == NULL) {
		perror("Error executing addr2line");
		exit(1);
	}
	
	char buffer[8192];
	buffer[8191] = 0;
	size_t length = fread(buffer, 1, 8191, addr2lineOutput);
	std::string cpp_buffer(buffer, length);
	pclose(addr2lineOutput);
	
	std::istringstream output(cpp_buffer);
	std::string function;
	std::string sourceLine;
	std::getline(output, function);
	std::getline(output, sourceLine);
	
	while (!output.eof()) {
		AddrInfoStep addInfoStep;
		
		if ((function != "??") && (sourceLine != "??:0") && (sourceLine != "??:?")) {
			auto functionIt = _sourceFunction2id.find(function);
			if (functionIt != _sourceFunction2id.end()) {
				addInfoStep._functionId = functionIt->second;
			} else {
				addInfoStep._functionId = _nextSourceFunctionId;
				_sourceFunction2id[function] = _nextSourceFunctionId;
				_id2sourceFunction[_nextSourceFunctionId] = function;
				_nextSourceFunctionId++;
			}
			
			auto sourceLineIt = _sourceLine2id.find(sourceLine);
			if (sourceLineIt != _sourceLine2id.end()) {
				addInfoStep._sourceLineId = sourceLineIt->second;
			} else {
				addInfoStep._sourceLineId = _nextSourceLineId;
				_sourceLine2id[sourceLine] = _nextSourceLineId;
				_id2sourceLine[_nextSourceLineId] = sourceLine;
				_nextSourceLineId++;
			}
			
			addrInfo.push_back(addInfoStep);
		}
		
		std::getline(output, function);
		std::getline(output, sourceLine);
	}
	
	_addr2Cache[address] = std::move(addrInfo);
	
	return _addr2Cache[address];
}


void Instrument::Profile::buildExecutableMemoryMap(pid_t pid)
{
	std::string mapsFilename;
	{
		std::ostringstream oss;
		oss << "/proc/" << pid << "/maps";
		mapsFilename = oss.str();
	}
	std::ifstream mapsFile(mapsFilename.c_str());
	
	if (!mapsFile.is_open()) {
		std::cerr << "Warning: cannot get the memory map of the process from '" << mapsFilename << "'" << std::endl;
		return;
	}
	
	std::istringstream splitter;
	std::string field;
	std::istringstream hexDecoder;
	hexDecoder.setf(std::ios::hex, std::ios::basefield);
	while (!mapsFile.eof() && !mapsFile.bad()) {
		std::string line;
		std::getline(mapsFile, line);
		
		if (mapsFile.eof()) {
			break;
		} else if (mapsFile.bad()) {
			std::cerr << "Warning: error getting the memory map of the process from '" << mapsFilename << "'" << std::endl;
			break;
		}
		
		splitter.clear();
		splitter.str(line);
		
		// Memory start address
		size_t baseAddress;
		std::getline(splitter, field, '-');
		hexDecoder.clear();
		hexDecoder.str(field);
		hexDecoder >> baseAddress;
		
		MemoryMapSegment &memoryMapSegment = _executableMemoryMap[(address_t) baseAddress];
		
		// Memory end address + 1
		std::getline(splitter, field, ' ');
		hexDecoder.clear();
		hexDecoder.str(field);
		hexDecoder >> memoryMapSegment._length;
		memoryMapSegment._length -= baseAddress;
		
		// Permissions
		std::getline(splitter, field, ' ');
		
		// Offset
		std::getline(splitter, field, ' ');
		hexDecoder.clear();
		hexDecoder.str(field);
		hexDecoder >> memoryMapSegment._offset;
		
		// Device
		std::getline(splitter, field, ' ');
		
		// Inode
		long inode;
		splitter >> inode;
		
		// Path (if any)
		std::string path;
		std::getline(splitter, path);
		{
			size_t beginningOfPath = path.find_first_not_of(' ');
			if (beginningOfPath != std::string::npos) {
				path = path.substr(beginningOfPath);
				if (!path.empty() && (path[0] != '[')) {
					memoryMapSegment._filename = std::move(path);
				}
			}
		}
	}
	
	mapsFile.close();
}


void Instrument::Profile::doShutdown()
{
	// After this, on the next profiling signal, the corresponding timer gets disarmed
	_enabled = false;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	
	#if !defined(HAVE_BACKTRACE) && !defined(HAVE_LIBUNWIND)
	return;
	#endif
	
	
	buildExecutableMemoryMap(getpid());
	
	
	// Build frequency tables and resolve address information
	std::map<address_t, freq_t> address2Frequency;
	std::map<Backtrace, freq_t> backtrace2Frequency;
	Backtrace backtrace(_profilingBacktraceDepth);
	int frame = 0;
	
	_bufferListSpinLock.lock();
	for (address_t *buffer : _bufferList) {
		long position = 0;
		frame = 0;
		
		while (position < _profilingBufferSize) {
			address_t address = buffer[position];
			
			if (address == 0) {
				if (frame == 0) {
					// End of buffer
					break;
				} else {
					// End of backtrace
					assert(frame <= _profilingBacktraceDepth);
					for (; frame < _profilingBacktraceDepth; frame++) {
						backtrace[frame] = 0;
					}
					
					// Increment the frequency of the backtrace
					auto it = backtrace2Frequency.find(backtrace);
					if (it == backtrace2Frequency.end()) {
						backtrace2Frequency[backtrace] = 1;
					} else {
						it->second++;
					}
					
					frame = 0;
					position++;
					continue;
				}
			}
			
			backtrace[frame] = address;
			frame++;
			
			{
				auto it = address2Frequency.find(address);
				if (it != address2Frequency.end()) {
					it->second++;
				} else {
					address2Frequency[address] = 1;
				}
			}
			
			AddrInfo const &addrInfo = resolveAddress(address);
			for (auto addrInfoStep : addrInfo) {
				_id2sourceFunction[addrInfoStep._functionId]._frequency++;
				_id2sourceLine[addrInfoStep._sourceLineId]._frequency++;
			}
			
			position++;
		}
		free(buffer);
	}
	_bufferList.clear();
	_bufferListSpinLock.unlock();
	
	
	std::map<freq_t, std::list<Backtrace>, std::greater<freq_t>> backtracesByFrequency;
	for (auto it : backtrace2Frequency) {
		backtracesByFrequency[it.second].push_back(it.first);
	}
	backtrace2Frequency.clear();
	
	{
		std::ostringstream oss;
		oss << "backtrace-profile-" << getpid() << ".txt";
		
		std::ofstream backtraceProfile(oss.str().c_str());
		for (auto it : backtracesByFrequency) {
			std::list<Backtrace> const &backtraces = it.second;
			for (Backtrace const &backtrace : backtraces) {
				bool first = true;
				for (address_t address : backtrace) {
					if (address == 0) {
						break;
					}
					
					AddrInfo const &addrInfo = resolveAddress(address);
					for (auto addrInfoStep : addrInfo) {
						if (first) {
							// Frequency on the innermost function
							backtraceProfile << it.first;
							first = false;
						}
						
						backtraceProfile << "\t" << _id2sourceFunction[addrInfoStep._functionId]._name;
						backtraceProfile << "\t" << _id2sourceLine[addrInfoStep._sourceLineId]._name;
						backtraceProfile << std::endl;
					}
				}
				
				if (!first) {
					backtraceProfile << std::endl;
				}
			}
		}
		backtraceProfile.close();
	}
	backtracesByFrequency.clear();
	
	
	std::map<freq_t, std::list<address_t>, std::greater<freq_t>> addressesByFrequency;
	for (auto it : address2Frequency) {
		addressesByFrequency[it.second].push_back(it.first);
	}
	address2Frequency.clear();
	
	{
		std::ostringstream oss;
		oss << "inline-profile-" << getpid() << ".txt";
		
		std::ofstream inlineProfile(oss.str().c_str());
		for (auto it : addressesByFrequency) {
			std::list<address_t> const &addresses = it.second;
			for (address_t address : addresses) {
				AddrInfo const &addrInfo = resolveAddress(address);
				if (!addrInfo.empty()) {
					// Frequency on the innermost function
					inlineProfile << it.first;
				}
				for (auto addrInfoStep : addrInfo) {
					inlineProfile << "\t" << _id2sourceFunction[addrInfoStep._functionId]._name;
					inlineProfile << "\t" << _id2sourceLine[addrInfoStep._sourceLineId]._name;
					inlineProfile << std::endl;
				}
			}
		}
		inlineProfile.close();
	}
	addressesByFrequency.clear();
	
	
	std::map<freq_t, std::list<id_t>, std::greater<freq_t>> functionsByFrequency;
	for (auto it : _id2sourceFunction) {
		functionsByFrequency[it.second. _frequency].push_back(it.first);
	}
	
	{
		std::ostringstream oss;
		oss << "function-profile-" << getpid() << ".txt";
		
		std::ofstream functionProfile(oss.str().c_str());
		for (auto it : functionsByFrequency) {
			std::list<id_t> const &functions = it.second;
			for (id_t functionId : functions) {
				NameAndFrequency const &function = _id2sourceFunction[functionId];
				functionProfile << function._frequency << "\t" << function._name << "\n";
			}
		}
		functionProfile.close();
	}
	
	std::map<freq_t, std::list<id_t>, std::greater<freq_t>> linesByFrequency;
	for (auto it : _id2sourceLine) {
		linesByFrequency[it.second. _frequency].push_back(it.first);
	}
	
	{
		std::ostringstream oss;
		oss << "line-profile-" << getpid() << ".txt";
		
		std::ofstream lineProfile(oss.str().c_str());
		for (auto it : linesByFrequency) {
			std::list<id_t> const &lines = it.second;
			for (id_t lineId : lines) {
				NameAndFrequency const &line = _id2sourceLine[lineId];
				lineProfile << line._frequency << "\t" << line._name << "\n";
			}
		}
		lineProfile.close();
	}
	
}
