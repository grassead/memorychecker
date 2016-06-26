#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <vector>
#include <string.h>
#include <pthread.h>
#include <malloc.h>


#define GUARD_PAGE_COUNT_BEFORE 0
#define GUARD_PAGE_COUNT_AFTER 0
#define PROTECT_USE_AFTER_FREE 0

#define CC_LIKELY(x)      __builtin_expect(!!(x), 1)
#define CC_UNLIKELY(x)    __builtin_expect(!!(x), 0)

#define PAGE_SIZE 4096

#define ALLOC_MAP_SIZE_MAX 1024*1024

static pthread_once_t isInitialized = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;


typedef struct allocInfo {
	void*  addr;
	void*  true_addr;
	size_t true_size;
	size_t size;
}allocInfo;


static allocInfo allocMap[ALLOC_MAP_SIZE_MAX];
static int       memFd = open("/dev/zero", 0);


static void init(void)
{
	memset(allocMap, 0, ALLOC_MAP_SIZE_MAX * sizeof(allocInfo));
}

static void computeTotalAlloc()
{
	int                i;
	long long unsigned internalSize = 0;
	long long unsigned userSize = 0;

	for (i = 0; i < ALLOC_MAP_SIZE_MAX; i++) {
		internalSize += allocMap[i].true_size;
		userSize     += allocMap[i].size;
	}
	printf("User size is %llu \n", userSize);
	printf("Total size is %llu \n", internalSize);
}

static void doAbort()
{
	computeTotalAlloc();
	abort();
}

static int addAlloc(allocInfo info)
{
	int i;
	int ret = -1;

	for (i = 0; i < ALLOC_MAP_SIZE_MAX; i++) {
		if (allocMap[i].true_addr == NULL) {
			allocMap[i] = info;
			ret = 0;
			goto exit;
		}
	}

exit:
	return ret;
}

static allocInfo* findAlloc(void* addr)
{
	int        i;
	allocInfo* ret = NULL;

	for (i = 0; i < ALLOC_MAP_SIZE_MAX; i++) {
		if ((allocMap[i]).addr == addr) {
			ret = &allocMap[i];
			goto exit;
		}
	}

exit:
	return ret;
}

static int alignSize(int align, int size)
{
	return ((int)(size / align)+1) * align;
}


static void* malloc_aligned(size_t align, size_t size)
{
	pthread_once(&isInitialized, init);

	void*  internalAddress = NULL;
	size_t beforeSize     = PAGE_SIZE * GUARD_PAGE_COUNT_BEFORE;

	void*  usrAddress = NULL;
	size_t userSize   = alignSize(align, size);

	void*  afterAddress = NULL;
	size_t afterSize    = PAGE_SIZE * GUARD_PAGE_COUNT_AFTER;

	size_t internalSize    = beforeSize + userSize + afterSize;


	int ret;
	allocInfo info;

	internalSize = userSize + ((GUARD_PAGE_COUNT_BEFORE + GUARD_PAGE_COUNT_AFTER )* PAGE_SIZE);

	internalAddress = mmap(NULL, internalSize, PROT_WRITE|PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, memFd, 0);
	if (internalAddress == MAP_FAILED) {
		printf("Mmap failed ! (%d)\n", errno);
		doAbort();
	}
	memset(internalAddress, 0, internalSize);

	usrAddress = (void*)((uint8_t*)internalAddress + beforeSize);

	afterAddress = (void*)((uint8_t*)usrAddress + userSize);


	if (beforeSize > 0) {
		ret = mprotect(internalAddress, beforeSize, PROT_NONE);
		if (CC_UNLIKELY(ret < 0)) {
			printf("mprotect (1) failed : %d\n", errno);
			doAbort();
		}
	}
	if (afterSize > 0) {
		ret = mprotect(afterAddress, afterSize, PROT_NONE);
		if (CC_UNLIKELY(ret < 0)) {
			printf("mprotect (2) failed : %d\n", errno);
			doAbort();
		}
	}

	info.true_addr = internalAddress;
	info.true_size = internalSize;
	info.addr = usrAddress;
	info.size = userSize;

	pthread_mutex_lock(&lock);
	ret = addAlloc(info);
	pthread_mutex_unlock(&lock);
	if (CC_UNLIKELY(ret < 0)) {
		printf("Too many alloc\n");
		doAbort();
	}

	return usrAddress;
}

extern "C" void* malloc(size_t size)
{
	return malloc_aligned(PAGE_SIZE, size);
}

extern "C" void* calloc(size_t nelem, size_t elemSize)
{
	void* alloc;
	size_t size = nelem * elemSize;

	alloc = malloc(size);

	memset(alloc, 0, size);
	return alloc;
}

static void free_locked(void* ptr)
{
	if (ptr == NULL) {
		return;
	}

	int ret;
	allocInfo* info;
	void* true_addr;
	size_t true_size;

	info = findAlloc(ptr);
	if (CC_UNLIKELY(info == NULL)) {
		printf("Bad free: %p\n", ptr);
		return;
	}


#if PROTECT_USE_AFTER_FREE
	ret = mprotect(info->true_addr, info->true_size, PROT_NONE);
#else
	ret = munmap(info->true_addr, info->true_size);
#endif
	if (ret != 0) {
		abort();
	}

	memset(info, 0, sizeof(allocInfo));


}

extern "C" void free(void* ptr)
{
	pthread_mutex_lock(&lock);
	free_locked(ptr);
	pthread_mutex_unlock(&lock);
}

extern "C" void* realloc(void* oldPtr, size_t newSize)
{

	void* newPtr = malloc(newSize);
	size_t copySize;
	allocInfo* info;

	if (oldPtr != NULL) {
		pthread_mutex_lock(&lock);
		info = findAlloc(oldPtr);
		if (CC_UNLIKELY(info == NULL)) {
			printf("Bad realloc !\n");
			doAbort();
		}

		if (info->size < newSize) {
			copySize = info->size;
		} else {
			copySize = newSize;
		}

		memcpy(newPtr, oldPtr, copySize);
		free_locked(oldPtr);
		pthread_mutex_unlock(&lock);
	}

	return newPtr;
}

extern "C" void* valloc(size_t size)
{
	return malloc(size);
}

#if 0
extern "C" int posix_memalign(void** memptr, size_t align, size_t size)
{
	printf("%s %d %d\n", __FUNCTION__, align, size);
	*memptr = malloc_aligned(align, size);
	printf("%d\n", (uintptr_t)(*memptr) % align);
	return 0;
}

extern "C" void* memalign(size_t align, size_t size)
{
	printf("%s\n", __FUNCTION__);
	abort();
}
#endif
