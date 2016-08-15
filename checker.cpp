#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
#include <pthread.h>
#include <malloc.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define ALIGN(x,a)              __ALIGN_MASK(x,(typeof(x))(a)-1)
#define __ALIGN_MASK(x,mask)    (((x)+(mask))&~(mask))

#define ALLOC_MAP_SIZE_MAX 1024*1024
#define DEFAULT_ALIGN 1
#define PAGE_SIZE 4096

#define PROTECT_BEFORE
//#define PROTECT_USE_AFTER_FREE

//#define printf(...)

typedef struct allocInfo {
	void*  usrAddr;
	size_t align;
	size_t usrSize;
	void*  trueAddr;
	size_t trueSize;
}allocInfo;


static void dumpInfo(allocInfo* info)
{
	printf("real address     = %p\n", info->trueAddr);
	printf("real size        = %ld\n", info->trueSize);
	printf("user end address = %p\n", (void*)((intptr_t)info->trueAddr + info->trueSize));
	printf("user address     = %p\n", info->usrAddr);
	printf("usr size         = %ld\n", info->usrSize);
	printf("user end address = %p\n", (void*)((intptr_t)info->usrAddr + info->usrSize));
	printf("align            = %ld\n", info->align);
}

static pthread_once_t isInitialized = PTHREAD_ONCE_INIT;
static allocInfo allocMap[ALLOC_MAP_SIZE_MAX];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void dumpAllocs()
{
	int i;

	printf("Dumping allocs ...\n");

	for (i = 0; i < ALLOC_MAP_SIZE_MAX; i++) {
		dumpInfo(&allocMap[i]);
	}
	
	printf("Dumping allocs done\n");
}

static void computeTotalAlloc()
{
	int                i;
	long long unsigned internalSize = 0;
	long long unsigned userSize = 0;

	for (i = 0; i < ALLOC_MAP_SIZE_MAX; i++) {
		internalSize += allocMap[i].trueSize;
		userSize     += allocMap[i].usrSize;
	}
	printf("User size is %llu \n", userSize);
	printf("Total size is %llu \n", internalSize);
}

static void doAbort()
{
	computeTotalAlloc();
	abort();
}


static void init(void)
{
	memset(allocMap, 0, ALLOC_MAP_SIZE_MAX * sizeof(allocInfo));
}

static int storeAlloc(allocInfo alloc)
{
	int i;
	int ret = -1;

	pthread_mutex_lock(&lock);

	for (i = 0; i < ALLOC_MAP_SIZE_MAX; i++) {
		if (allocMap[i].usrAddr == NULL) {
			allocMap[i] = alloc;
			ret = 0;
			goto exit;
		}
	}

exit:
	pthread_mutex_unlock(&lock);
	return ret;
}

static allocInfo* getAlloc(void* userAddr)
{
	int         i;
	allocInfo* ret = NULL;

	for (i = 0; i < ALLOC_MAP_SIZE_MAX; i++) {
		if (allocMap[i].usrAddr == userAddr) {
			ret = &allocMap[i];
			goto exit;
		}
	}

exit:
	return ret;
}

static size_t getAllocSize(size_t userSize)
{
	return ALIGN(userSize, PAGE_SIZE) + PAGE_SIZE;
}

static void protect(allocInfo* alloc)
{
	void* beginAddr;
	int pageCount;
	int ret;

	pageCount = (int)((intptr_t)alloc->usrAddr - (intptr_t)alloc->trueAddr) / PAGE_SIZE;
	//printf("Page Count before = %d\n", pageCount);
	beginAddr = alloc->trueAddr;
	if (pageCount > 0) {
		//printf("protecting from %p to %p\n", beginAddr, (void*)((intptr_t)beginAddr + pageCount * PAGE_SIZE));
		ret = mprotect(beginAddr, pageCount * PAGE_SIZE, PROT_NONE);
		if (ret != 0) {
			printf("mprotect failed with %d\n", errno);
			doAbort();
		}
	}

	pageCount = (int)((((intptr_t)alloc->trueAddr + alloc->trueSize) - ((intptr_t)alloc->usrAddr + alloc->usrSize)) / 4096);
	//printf("Page Count after = %d\n", pageCount);
	beginAddr = (void*)((intptr_t)alloc->usrAddr + alloc->usrSize);
	beginAddr = (void*)ALIGN((intptr_t)beginAddr, PAGE_SIZE);
	if (pageCount > 0) {
		//printf("protecting from %p to %p\n", beginAddr, (void*)((intptr_t)beginAddr + pageCount * PAGE_SIZE));
		ret = mprotect(beginAddr, pageCount * PAGE_SIZE, PROT_NONE);
		if (ret != 0) {
			printf("mprotect failed with %d\n", errno);
			doAbort();
		}
	}
}

static void* doAlloc(const size_t size)
{
	void* ret;
	ret = mmap(NULL, size, PROT_WRITE|PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (ret == MAP_FAILED) {
		printf("mmap failed: %d (size is %ld)\n", errno, size);
		doAbort();
	}
	return ret;
}

static intptr_t getAlignedAddress(intptr_t base, size_t align)
{
	return (base + align -1) / align * align;
}

static void* getUsrAddress(allocInfo* alloc, size_t align)
{
#ifdef PROTECT_BEFORE
	return (void*)getAlignedAddress((intptr_t)alloc->trueAddr + PAGE_SIZE, align);
#else
	return (void*)getAlignedAddress((intptr_t)alloc->trueAddr + alloc->trueSize - PAGE_SIZE - alloc->usrSize - alloc->align, align);
#endif
}

void* alloc(size_t size, size_t align)
{
	int    ret;
	allocInfo info;
	
	memset(&info, 0, sizeof(allocInfo));

	if (size == 0) {
		return NULL;
	}

	pthread_once(&isInitialized, init);

	info.usrSize = size;

	//If a special alignement is needed, alloc extra space
	if (align != DEFAULT_ALIGN) {
		info.trueSize += align;
		info.align = align;
	}

	info.trueSize += size;
	info.trueSize = getAllocSize(info.trueSize);
	info.trueAddr = doAlloc(info.trueSize);

	info.usrAddr = getUsrAddress(&info, align);
	
	protect(&info);

	ret = storeAlloc(info);
	assert(ret == 0);
	//dumpInfo(&info);

	return info.usrAddr;
}

void releaseLocked(void* address)
{
	allocInfo* info;

	info = getAlloc(address);
	
	if (info != NULL) {
#ifdef PROTECT_USE_AFTER_FREE
		mprotect((info)->trueAddr, (info)->trueSize, PROT_NONE);
#else
		munmap((info)->trueAddr, (info)->trueSize);
#endif
		memset(info, 0, sizeof(allocInfo));
	}
}

void release(void* address)
{
	pthread_mutex_lock(&lock);
	releaseLocked(address);
	pthread_mutex_unlock(&lock);
}

#define printf(...)

extern "C" void* malloc(size_t size)
{
	void* ret;
	printf("%s: begin size = %ld\n", __FUNCTION__, size);
	ret = alloc(size, DEFAULT_ALIGN);
	printf("%s: end with %p\n", __FUNCTION__, ret);
	return ret;
}

extern "C" void* calloc(size_t nelem, size_t elemSize)
{
	void* ret;
	size_t size = nelem * elemSize;
	printf("%s: begin\n", __FUNCTION__);

	ret = alloc(size, DEFAULT_ALIGN);

	memset(ret, 0, size);
	printf("%s: end\n", __FUNCTION__);
	return ret;
}

extern "C" void free(void* ptr)
{
	printf("%s: begin\n", __FUNCTION__);
	release(ptr);
	printf("%s: end\n", __FUNCTION__);
}

extern "C" void* realloc(void* oldPtr, size_t newSize)
{
	void* newPtr = alloc(newSize, DEFAULT_ALIGN);
	size_t copySize;
	allocInfo* info;
	printf("%s: begin\n", __FUNCTION__);
	
	if (oldPtr != NULL) {
		pthread_mutex_lock(&lock);
		info = getAlloc(oldPtr);
		assert (info != NULL);

		if ((info)->usrSize < newSize) {
			copySize = (info)->usrSize;
		} else {
			copySize = newSize;
		}

		memcpy(newPtr, oldPtr, copySize);
		releaseLocked(oldPtr);
		pthread_mutex_unlock(&lock);
	}

	printf("%s: end\n", __FUNCTION__);
	return newPtr;
}

extern "C" void* valloc(size_t size)
{
	void* ret;
	printf("%s: begin\n", __FUNCTION__);
	ret = alloc(size, DEFAULT_ALIGN);
	printf("%s: end\n", __FUNCTION__);
	return ret;
}

extern "C" int posix_memalign(void** memptr, size_t align, size_t size)
{
	printf("%s: begin\n", __FUNCTION__);
	*memptr = alloc(size, align);
	printf("%s: end\n", __FUNCTION__);
	return 0;
}

extern "C" void* memalign(size_t align, size_t size)
{
	void* ret;
	printf("%s: begin\n", __FUNCTION__);
	ret = alloc(size, align);
	printf("%s: end\n", __FUNCTION__);
	return ret;
}


/*
int main (void)
{
	int* ptr;
	int i;
	
	ptr = (int*)alloc(10 * sizeof(int), 4097);


	if ((intptr_t)ptr % (4097) == 0) {
		printf("Aligned\n");
	} else {
		printf("Not aligned\n");
	}

	printf("ptr = %p\n", ptr);

	for (i = 0; i < 10; i++) {
		printf("Writing i = %d\n", i);
		ptr[i] = 10;
	}


	release(ptr);

	return 0;
}
*/
