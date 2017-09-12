/*
	This file is part of Nanos6 and is licensed under the terms contained in the COPYING file.
	
	Copyright (C) 2017 Barcelona Supercomputing Center (BSC)
*/

#ifndef NDEBUG
#include <loader/malloc.h>
#include <lowlevel/SymbolResolver.hpp>

#include <lowlevel/EnvironmentVariable.hpp>
#include <lowlevel/FatalErrorHandler.hpp>

#include <algorithm>

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

static EnvironmentVariable<bool> _debugMemory("NANOS6_DEBUG_MEMORY", false);
static EnvironmentVariable<bool> _protectAfter("NANOS6_DEBUG_MEMORY_PROTECT_AFTER", true);
static EnvironmentVariable<size_t> _guardPages("NANOS6_DEBUG_MEMORY_GUARD_PAGES", 1);
static int _pageSize = 0;
static bool _noFree = false;


// Allocation Schema:
// 
// [Padding Pages for Alignment]
// [memory_allocation_info_t Page]
// [Protected Page #1]
//    ...
// [Protected Page #_guardPages]
// [ACTUAL DATA PAGES]
// [Protected Page #_guardPages+1]
//    ...
// [Protected Page #_guardPages+#_guardPages]
// [Padding Pages for Alignment]


struct memory_allocation_info_t {
	char _magic[64];
	
	void *_blockStart;
	size_t _blockLength;
	
	size_t _padding1Size;
	
	void *_firstUserPage;
	void *_userStart;
	size_t _requestedSize;
	
	void *_protected2Start;
	
	bool _deallocated;
	
	// Backup copy of the information
	void *_backupOfBlockStart;
	size_t _backupOfBlockLength;
	size_t _backupOfPadding1Size;
	void *_backupOfFirstUserPage;
	void *_backupOfUserStart;
	size_t _backupOfRequestedSize;
	void *_backupOfProtected2Start;
	
	bool _backupOfDeallocated;
	
	
	memory_allocation_info_t()
	{
		strcpy(_magic, "NANOS6 MEMORY DEBUGGING INFORMATION");
	}
	
	bool isOurs() const
	{
#if 0
		if (_magic[0] != 'N') {
			return false;
		}
		if (_magic[1] != 'A') {
			return false;
		}
		if (_magic[2] != 'N') {
			return false;
		}
		if (_magic[3] != 'O') {
			return false;
		}
		return (strcmp(_magic, "NANOS6 MEMORY DEBUGGING INFORMATION") == 0);
#else
		return true;
#endif
	}
	
	void setUpConsistencyInformation()
	{
		_backupOfBlockStart = _blockStart;
		_backupOfBlockLength = _blockLength;
		_backupOfPadding1Size = _padding1Size;
		_backupOfFirstUserPage = _firstUserPage;
		_backupOfUserStart = _userStart;
		_backupOfRequestedSize = _requestedSize;
		_backupOfProtected2Start = _protected2Start;
		_backupOfDeallocated = _deallocated;
	}
	
	void verifyConsistency() const
	{
		FatalErrorHandler::check(_backupOfBlockStart == _blockStart, "Detected corruption in the memory allocation registry");
		FatalErrorHandler::check(_backupOfBlockLength == _blockLength, "Detected corruption in the memory allocation registry");
		FatalErrorHandler::check(_backupOfPadding1Size == _padding1Size, "Detected corruption in the memory allocation registry");
		FatalErrorHandler::check(_backupOfFirstUserPage == _firstUserPage, "Detected corruption in the memory allocation registry");
		FatalErrorHandler::check(_backupOfUserStart == _userStart, "Detected corruption in the memory allocation registry");
		FatalErrorHandler::check(_backupOfRequestedSize == _requestedSize, "Detected corruption in the memory allocation registry");
		FatalErrorHandler::check(_backupOfProtected2Start == _protected2Start, "Detected corruption in the memory allocation registry");
		FatalErrorHandler::check(_backupOfDeallocated == _deallocated, "Detected corruption in the memory allocation registry");
	}
};


union pointer_arithmetic_t {
	void *_void_pointer;
	memory_allocation_info_t *_allocation_info_pointer;
	size_t _offset;
};


__attribute__((constructor(0)))
static void nanos6_memory_allocation_interception_init()
{
	_pageSize = sysconf(_SC_PAGE_SIZE);
	SymbolResolver<void>::globalScopeCall("nanos6_start_function_interception");
}


void nanos6_memory_allocation_interception_fini()
{
	// Since some libraries may have been loaded before the interception, we do not know how distinguish which memory comes from where
	_noFree = true;
	
// 	SymbolResolver<void>::globalScopeCall("nanos6_stop_function_interception");
}


static size_t nanos6_calculate_memory_allocation_size(size_t requestedSize, size_t alignment = sizeof(void *))
{
	if (alignment > sizeof(void *)) {
		requestedSize += alignment*2;
	}
	
	size_t actualSize = (requestedSize + _pageSize + _pageSize*_guardPages + _pageSize*_guardPages + _pageSize - 1) / _pageSize;
	actualSize = actualSize * _pageSize;
	assert(actualSize % _pageSize == 0);
	
	return actualSize;
}


static memory_allocation_info_t *nanos6_protected_memory_get_allocation_info(void *address, bool returnNullIfNotOurs = false)
{
	pointer_arithmetic_t pointer;
	pointer._void_pointer = address;
	
	size_t pageMissalignment = pointer._offset % _pageSize;
	pointer._offset -= pageMissalignment;
	pointer._offset -= _pageSize * _guardPages;
	pointer._offset -= _pageSize;
	
	if (returnNullIfNotOurs) {
		if (!pointer._allocation_info_pointer->isOurs()) {
			return nullptr;
		}
	}
	
	pointer._allocation_info_pointer->verifyConsistency();
	
	return pointer._allocation_info_pointer;
}


static void *nanos6_protected_memory_allocation(size_t requestedSize, size_t alignment = sizeof(void *))
{
	size_t actualSize = nanos6_calculate_memory_allocation_size(requestedSize, alignment);
	
	void *memory = mmap(nullptr, actualSize, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	FatalErrorHandler::check(memory != nullptr, "Cannot allocate ", actualSize, " bytes for an allocation of ", requestedSize, " bytes");
	
	pointer_arithmetic_t pointer;
	pointer._void_pointer = memory;
	pointer._offset += _pageSize; // For memory_allocation_info_t
	pointer._offset += _pageSize*_guardPages; // Leading protected pages
	
	size_t requestedPages = (requestedSize + _pageSize - 1) / _pageSize;
	size_t pagePadding1 = 0;
	size_t subpageExtraAlignment = 0;
	
	size_t missalignment;
	if (_protectAfter) {
		missalignment = (pointer._offset + requestedPages*_pageSize - requestedSize) % alignment;
		pagePadding1 = requestedPages*_pageSize - requestedSize - missalignment;
	} else {
		missalignment = pointer._offset % alignment;
		pagePadding1 = alignment - missalignment;
	}
	subpageExtraAlignment = pagePadding1 % _pageSize;
	pagePadding1 = pagePadding1 - subpageExtraAlignment;
	
	assert(pagePadding1 + _pageSize + _pageSize*_guardPages + subpageExtraAlignment + requestedSize + _pageSize*_guardPages <= actualSize);
	
	pointer._void_pointer = memory;
	
	if (pagePadding1 != 0) {
		int rc = mprotect(pointer._void_pointer, pagePadding1, PROT_NONE);
		FatalErrorHandler::handle(rc, "Failed to protect the leading alignment padding during a memory allocation");
		pointer._offset += pagePadding1;
	}
	
	memory_allocation_info_t *allocationInfo = (memory_allocation_info_t *) pointer._void_pointer;
	new (allocationInfo) memory_allocation_info_t();
	pointer._offset += _pageSize;
	
	int rc = mprotect(pointer._void_pointer, _pageSize * _guardPages, PROT_NONE);
	FatalErrorHandler::handle(rc, "Failed to protect the leading guard page(s) during a memory allocation");
	pointer._offset += _pageSize * _guardPages;
	
	void *firstUserPage = pointer._void_pointer;
	
	pointer._offset += subpageExtraAlignment;
	void *result = pointer._void_pointer;
	assert(pointer._offset % alignment == 0);
	
	pointer._offset += requestedSize;
	{
		size_t pageMissalignment = pointer._offset % _pageSize;
		if (pageMissalignment != 0) {
			pointer._offset += _pageSize - pageMissalignment;
		}
	}
	
	rc = mprotect(pointer._void_pointer, _pageSize * _guardPages, PROT_NONE);
	FatalErrorHandler::handle(rc, "Failed to protect the trailing guard page(s) during a memory allocation");
	
	allocationInfo->_blockStart = memory;
	allocationInfo->_blockLength = actualSize;
	allocationInfo->_padding1Size = pagePadding1;
	allocationInfo->_firstUserPage = firstUserPage;
	allocationInfo->_userStart = result;
	allocationInfo->_requestedSize = requestedSize;
	allocationInfo->_protected2Start = pointer._void_pointer;
	allocationInfo->_deallocated = false;
	allocationInfo->setUpConsistencyInformation();
	
	// Check that we can get back to the allocation info
	assert(nanos6_protected_memory_get_allocation_info(result) == allocationInfo);
	
	return result;
}


static void nanos6_protected_memory_deallocation(void *address)
{
	if (_noFree) {
		return;
	}
	
	memory_allocation_info_t *allocationInfo = nanos6_protected_memory_get_allocation_info(address, true);
	if (allocationInfo == nullptr) {
		// Not ours. May have been allocated before the memory allocation functions were intercepted.
		SymbolResolver<void, void *>::call("free", address);
		return;
	}
	
	FatalErrorHandler::check(!allocationInfo->_deallocated, "Attempt to free memory twice");
	
	size_t userPagesSize = ((size_t) allocationInfo->_protected2Start) - ((size_t) allocationInfo->_firstUserPage);
	int rc = madvise(allocationInfo->_firstUserPage, userPagesSize, MADV_FREE);
	FatalErrorHandler::handle(rc, "Failed to discard pages during memory deallocation");
	
	rc = mprotect(allocationInfo->_firstUserPage, userPagesSize, PROT_NONE);
	FatalErrorHandler::handle(rc, "Failed to protect pages during memory deallocation");
	
	allocationInfo->_deallocated = true;
}



void *nanos6_intercepted_malloc(size_t size)
{
	if (_debugMemory) {
		return nanos6_protected_memory_allocation(size);
	} else {
		return SymbolResolver<void *, size_t>::call("malloc", size);
	}
}

void nanos6_intercepted_free(void *ptr)
{
	if (ptr != nullptr) {
		if (_debugMemory) {
			nanos6_protected_memory_deallocation(ptr);
		} else {
			SymbolResolver<void, void *>::call("free", ptr);
		}
	}
}

void *nanos6_intercepted_calloc(size_t nmemb, size_t size)
{
	if (_debugMemory) {
		void *result = nanos6_protected_memory_allocation(nmemb * size, size);
		return memset(result, 0, nmemb* size);
	} else {
		return SymbolResolver<void *, size_t, size_t>::call("calloc", nmemb, size);
	}
}

void *nanos6_intercepted_realloc(void *ptr, size_t size)
{
	if (_debugMemory) {
		void *result = nanos6_protected_memory_allocation(size);
		if (ptr != nullptr) {
			if (size != 0) {
				memory_allocation_info_t *allocationInfo = nanos6_protected_memory_get_allocation_info(ptr);
				FatalErrorHandler::check(!allocationInfo->_deallocated, "Attempt to realloc freed memory");
				memcpy(result, ptr, std::min(allocationInfo->_requestedSize, size));
			}
			nanos6_protected_memory_deallocation(ptr);
		}
		return result;
	} else {
		return SymbolResolver<void *, void *, size_t>::call("realloc", ptr, size);
	}
}

void *nanos6_intercepted_reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (_debugMemory) {
		void *result = nanos6_protected_memory_allocation(nmemb*size, size);
		if (ptr != nullptr) {
			if (nmemb*size != 0) {
				memory_allocation_info_t *allocationInfo = nanos6_protected_memory_get_allocation_info(ptr);
				FatalErrorHandler::check(!allocationInfo->_deallocated, "Attempt to reallocallar freed memory");
				memcpy(result, ptr, std::min(allocationInfo->_requestedSize, nmemb*size));
			}
			nanos6_protected_memory_deallocation(ptr);
		}
		return result;
	} else {
		return SymbolResolver<void *, void *, size_t, size_t>::call("reallocarray", ptr, nmemb, size);
	}
}


int nanos6_intercepted_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (_debugMemory) {
		*memptr = nanos6_protected_memory_allocation(size, alignment);
		return 0;
	} else {
		return SymbolResolver<int, void **, size_t, size_t>::call("posix_memalign", memptr, alignment, size);
	}
}


void *nanos6_intercepted_aligned_alloc(size_t alignment, size_t size)
{
	if (_debugMemory) {
		return nanos6_protected_memory_allocation(size, alignment);
	} else {
		return SymbolResolver<void *, size_t, size_t>::call("aligned_alloc", alignment, size);
	}
}


void *nanos6_intercepted_valloc(size_t size)
{
	if (_debugMemory) {
		if (_pageSize == 0) {
			_pageSize = sysconf(_SC_PAGE_SIZE);
		}
		
		return nanos6_protected_memory_allocation(size, _pageSize);
	} else {
		return SymbolResolver<void *, size_t>::call("valloc", size);
	}
}


void *nanos6_intercepted_memalign(size_t alignment, size_t size)
{
	if (_debugMemory) {
		return nanos6_protected_memory_allocation(size, alignment);
	} else {
		return SymbolResolver<void *, size_t, size_t>::call("memalign", alignment, size);
	}
}


void *nanos6_intercepted_pvalloc(size_t size)
{
	if (_debugMemory) {
		if (_pageSize == 0) {
			_pageSize = sysconf(_SC_PAGE_SIZE);
		}
		
		size = (size + _pageSize - 1) / _pageSize;
		
		return nanos6_protected_memory_allocation(size);
	} else {
		return SymbolResolver<void *, size_t>::call("pvalloc", size);
	}
}
#endif

