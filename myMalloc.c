#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myMalloc.h"
#include "printing.h"

/* Due to the way assert() prints error messges we use out own assert function
 * for deteminism when testing assertions
 */
#ifdef TEST_ASSERT
inline static void assert(int e) {
	if (!e) {
		const char * msg = "Assertion Failed!\n";
		write(2, msg, strlen(msg));
		exit(1);
	}
}
#else
#include <assert.h>
#endif

/*
 * Mutex to ensure thread safety for the freelist
 */
static pthread_mutex_t mutex;

/*
 * Array of sentinel nodes for the freelists
 */
header freelistSentinels[N_LISTS];

/*
 * Pointer to the second fencepost in the most recently allocated chunk from
 * the OS. Used for coalescing chunks
 */
header * lastFencePost;

/*
 * Pointer to maintian the base of the heap to allow printing based on the
 * distance from the base of the heap
 */ 
void * base;

/*
 * List of chunks allocated by  the OS for printing boundary tags
 */
header * osChunkList [MAX_OS_CHUNKS];
size_t numOsChunks = 0;

/*
 * direct the compiler to run the init function before running main
 * this allows initialization of required globals
 */
static void init (void) __attribute__ ((constructor));

// Helper functions for manipulating pointers to headers
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off);
static inline header * get_left_header(header * h);
static inline header * ptr_to_header(void * p);

// Helper functions for allocating more memory from the OS
static inline void initialize_fencepost(header * fp, size_t left_size);
static inline void insert_os_chunk(header * hdr);
static inline void insert_fenceposts(void * raw_mem, size_t size);
static header * allocate_chunk(size_t size);

// Helper functions for freeing a block
static inline void deallocate_object(void * p);

// Helper functions for allocating a block
static inline header * allocate_object(size_t raw_size);

// Helper functions for verifying that the data structures are structurally 
// valid
static inline header * detect_cycles();
static inline header * verify_pointers();
static inline bool verify_freelist();
static inline header * verify_chunk(header * chunk);
static inline bool verify_tags();

static void init();

static bool isMallocInitialized;


static inline void insert_helper (header* h)
{
	header *sentinel;
	if(get_size(h) / 8 -3 >= 58)
	{
		sentinel = &freelistSentinels[58];
	}
	else 
	{
		sentinel = &freelistSentinels[get_size(h)/8-3];
	}

	sentinel->next->prev = h;
	h->next = sentinel->next;
	sentinel->next = h;
	h->prev = sentinel;
}

static inline void break_helper(header *h)
{
	h->prev->next = h->next;
	h->next->prev = h->prev;
	h->next = NULL;
	h->prev = NULL;
}

/**
 * @brief Helper function to retrieve a header pointer from a pointer and an 
 *        offset
 *
 * @param ptr base pointer
 * @param off number of bytes from base pointer where header is located
 *
 * @return a pointer to a header offset bytes from pointer
 */
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off) {
	return (header *)((char *) ptr + off);
}

/**
 * @brief Helper function to get the header to the right of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
header * get_right_header(header * h) {
	return get_header_from_offset(h, get_size(h));
}

/**
 * @brief Helper function to get the header to the left of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
inline static header * get_left_header(header * h) {
	return get_header_from_offset(h, -h->left_size);
}

/**
 * @brief Fenceposts are marked as always allocated and may need to have
 * a left object size to ensure coalescing happens properly
 *
 * @param fp a pointer to the header being used as a fencepost
 * @param left_size the size of the object to the left of the fencepost
 */
inline static void initialize_fencepost(header * fp, size_t left_size) {
	set_state(fp,FENCEPOST);
	set_size(fp, ALLOC_HEADER_SIZE);
	fp->left_size = left_size;
}

/**
 * @brief Helper function to maintain list of chunks from the OS for debugging
 *
 * @param hdr the first fencepost in the chunk allocated by the OS
 */
inline static void insert_os_chunk(header * hdr) {
	if (numOsChunks < MAX_OS_CHUNKS) {
		osChunkList[numOsChunks++] = hdr;
	}
}

/**
 * @brief given a chunk of memory insert fenceposts at the left and 
 * right boundaries of the block to prevent coalescing outside of the
 * block
 *
 * @param raw_mem a void pointer to the memory chunk to initialize
 * @param size the size of the allocated chunk
 */
inline static void insert_fenceposts(void * raw_mem, size_t size) {
	// Convert to char * before performing operations
	char * mem = (char *) raw_mem;

	// Insert a fencepost at the left edge of the block
	header * leftFencePost = (header *) mem;
	initialize_fencepost(leftFencePost, ALLOC_HEADER_SIZE);

	// Insert a fencepost at the right edge of the block
	header * rightFencePost = get_header_from_offset(mem, size - ALLOC_HEADER_SIZE);
	initialize_fencepost(rightFencePost, size - 2 * ALLOC_HEADER_SIZE);
}

/**
 * @brief Allocate another chunk from the OS and prepare to insert it
 * into the free list
 *
 * @param size The size to allocate from the OS
 *
 * @return A pointer to the allocable block in the chunk (just after the 
 * first fencpost)
 */
static header * allocate_chunk(size_t size) {
	void * mem = sbrk(size);

	insert_fenceposts(mem, size);
	header * hdr = (header *) ((char *)mem + ALLOC_HEADER_SIZE);
	set_state(hdr, UNALLOCATED);
	set_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
	hdr->left_size = ALLOC_HEADER_SIZE;
	return hdr;
}

/**
 * @brief Helper allocate an object given a raw request size from the user
 *
 * @param raw_size number of bytes the user needs
 *
 * @return A block satisfying the user's request
 */
static inline header * allocate_object(size_t raw_size) {
	// TODO implement allocation
	(void) raw_size;
	//assert(false);
	//exit(1);

	// 16 = ALLOC_HEADER_SIZE
	// 32 = sizeof(header)

	//size_t temp = raw_size % 8;
	size_t required;
	if (raw_size == 0)
	{
		return NULL;
	}
	if ((raw_size+16) % 8 == 0)
	{
		//
		required = raw_size + 16;

	}
	else
	{
		required = (((raw_size + 16)/ 8) + 1) * 8;
	}

	//new allocated 
	header *split;
	header *new;
	header *block;


	if (required <= 32)
	{
		required = 32;
	}

	for (int i = 0; i < N_LISTS; i++)
	{
		header *nonempty = &freelistSentinels[i];

		if (get_size(nonempty->next)== required)
		{
			block  = nonempty->next;

			break_helper(block);
			set_state(block, ALLOCATED);
			return (void*)&block->data[0];

		}
		//check for none empty linked list sentinel;
		else if (i < 58 && get_size(nonempty->next) > required)
		{	
			//get the sentinal, set block points to the first block in the list
			new  = &freelistSentinels[i];
			block  = new->next;

			break_helper(block);
			//set the size and state
			set_size(block, get_size(block)-required);

			//add to new freelist
			insert_helper(block);

			//set new pointer to new allocated location
			split = get_right_header(block);

			//set size and state
			set_size(split, required);
			set_state(split, ALLOCATED);

			//update split left size
			split->left_size = get_size(block);

			//update the right header's left size
			get_right_header(split)->left_size = get_size(split);

			return (void*)&split->data[0];
		}
		else if (i == 58 && get_size(nonempty->next) != 0)
		{
			//check the final list by iterating throught all the block
			block = nonempty->next;
			while (get_size(block) != 0)
			{
				if (get_size(block) > required)
				{
					if(get_size(block) - required >= 488)
					{
						set_size(block,get_size(block)-required);
					}
					else{
					//break pointers
					break_helper(block);
					//set the size and state
					set_size(block, get_size(block) - required);
					//add to new freelist
					insert_helper(block);
					}
					//set new pointer to new allocated location
					split = get_right_header(block);

					//set size and state
					set_size(split, required);
					set_state(split, ALLOCATED);

					//update its split left size
					split->left_size = get_size(block);

					//update the right header's left size
					get_right_header(split)->left_size = get_size(split);

					return (void*)&split->data[0];
				}
				else if (get_size(block)== required)
				{
					break_helper(block);
					set_state(block, ALLOCATED);
					return (void*)&block->data[0];

				}
				block = block->next;
			}
		}
	}

	header *new_chunk = allocate_chunk(ARENA_SIZE);
	
	
	//new_chunk is not adjcent to the previous one;
	if (get_left_header(get_left_header(new_chunk)) != lastFencePost)
	{
		insert_os_chunk(get_left_header(new_chunk));

		set_size(new_chunk,  get_size(new_chunk) - required);
		set_state(new_chunk, UNALLOCATED);
		insert_helper(new_chunk);

		header *new_allocated = get_right_header(new_chunk);

		//set size and state
		set_size(new_allocated, required);
		set_state(new_allocated, ALLOCATED);
		//update split left size
		new_allocated->left_size = get_size(new_chunk);

		//update the right header's left size
		lastFencePost = get_right_header(new_allocated);
		lastFencePost->left_size = get_size(new_allocated);
		return (void*)&new_allocated->data[0];
	}

	//new_chunk is adjcent to the previous one
	else
	{
		//previous chunk is allocated
		if (get_state(get_left_header(lastFencePost)) == ALLOCATED)
		{
			/*try: dont' break out lastfencepost, pointer start from lastfencepost and merge
			  insert new_chunk into freelist;

			  make a new pointer that points to the new allocated chunk
			  set its size, set the state to ALLOCATED
			  update its left size;
			  update its right size;
			 */
			header *n;
			n = lastFencePost;
			set_size(n, get_size(n) + get_size(get_left_header(new_chunk)) + get_size(new_chunk) - required);
			set_state(n, UNALLOCATED);
			insert_helper(n);

			header *new_allocated;
			new_allocated = get_right_header(n);
			set_size(new_allocated, required);
			set_state(new_allocated,ALLOCATED);

			new_allocated->left_size = get_size(n);

			//get_right_header(new_allocated)->left_size = get_size(new_allocated);

			lastFencePost = get_right_header(new_allocated);
			lastFencePost->left_size = get_size(new_allocated);
			return (void*)&new_allocated->data[0];

		}
		//prev chunk is unallocated
		else if (get_state(get_left_header(lastFencePost)) == UNALLOCATED)
		{
			/*use get_left_header(get_left_header(get_left_header(new_chunk)))
			  unallocated_object --> lastfencepost --> new_fenceopost --> new_chunk;

			  break the unallocated_object from freelist
			  merge from the unallocated_object;
			  insert the rest of the chunk into freelist;

			  make a new pointer that points to the new allocated chunk;
			  set size, state, left_header, right_header;
			 */
			header *left = get_left_header(get_left_header(get_left_header(new_chunk)));
			break_helper(get_left_header(get_left_header(get_left_header(new_chunk))));
			set_size(left, get_size(left) + get_size(get_left_header(get_left_header(new_chunk))) + get_size(get_left_header(new_chunk)) + get_size(new_chunk) - required);
			set_state(left, UNALLOCATED);
			insert_helper(left); 
			header *new_allocated;
			new_allocated = get_right_header(left);
			set_size(new_allocated, required);
			set_state(new_allocated, ALLOCATED);

			new_allocated->left_size = get_size(left);

			get_right_header(new_allocated)->left_size = get_size(new_allocated);
			lastFencePost = get_right_header(new_allocated);
			return (void*)&new_allocated->data[0];
		}
		
	
	}
}

/**
 * @brief Helper to get the header from a pointer allocated with malloc
 *
 * @param p pointer to the data region of the block
 *
 * @return A pointer to the header of the block
 */
static inline header * ptr_to_header(void * p) {
	return (header *)((char *) p - ALLOC_HEADER_SIZE); //sizeof(header));
}

/**
 * @brief Helper to manage deallocation of a pointer returned by the user
 *
 * @param p The pointer returned to the user by a call to malloc
 */
static inline void deallocate_object(void * p) {
	// TODO implement deallocation
	//assert(false);
	//exit(1);
	
	if (p == NULL)
	{
		return;
	}

	//header *d = ptr_to_header(p);
	header *d = (header *) ((char *) p - ALLOC_HEADER_SIZE);
	if (get_state(d) == UNALLOCATED)
	{
		fprintf(stderr, "Double Free Detected\n");
		assert(false);
		exit(1);
	}

	set_state(d, UNALLOCATED);

	if (get_state(get_right_header(d)) != UNALLOCATED && get_state(get_left_header(d)) != UNALLOCATED)
	{
		//insert into free list;
		insert_helper(d);
	}
	else if (get_state(get_right_header(d)) == UNALLOCATED && get_state(get_left_header(d)) != UNALLOCATED)
	{

			
		break_helper(get_right_header(d));

		//coalesce
		set_size(d, get_size(d) + get_size(get_right_header(d)));

		//change left size
		get_right_header(d)->left_size = get_size(d);

		//insert into free list;
		insert_helper(d);
	}
	else if (get_state(get_right_header(d)) != UNALLOCATED && get_state(get_left_header(d)) == UNALLOCATED)
	{
		header * left = get_left_header(d);
		if (get_size(get_left_header(d)) >= 488)
		{
			set_size(left, get_size(get_left_header(d)) + get_size(d));
			//change left size
			get_right_header(left)->left_size = get_size(left);
		}
		else
		{
			break_helper(get_left_header(d));
			set_size(left, get_size(get_left_header(d)) + get_size(d));	
			//change left size
			get_right_header(left)->left_size = get_size(left);
			insert_helper(get_left_header(d));
		}
	}

	else if (get_state(get_right_header(d)) == UNALLOCATED && get_state(get_left_header(d)) == UNALLOCATED)
	{
		header *left = get_left_header(d);
		if (get_size(get_left_header(d)) >= 488)
		{
			//break out right_header
                	break_helper(get_right_header(d));
			//coalesce
                        set_size(left, get_size(left) + get_size(d) + get_size(get_right_header(d)));
			//change left size
                        get_right_header(left)->left_size = get_size(left);

		}
		else{
			break_helper(get_left_header(d));
			//break out right_header
			break_helper(get_right_header(d));
			//coalesce
			set_size(left, get_size(left) + get_size(d) + get_size(get_right_header(d)));
			//change left size
			get_right_header(left)->left_size = get_size(left);
			insert_helper(get_left_header(d));
		}
	} 


}

/**
 * @brief Helper to detect cycles in the free list
 * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
 *
 * @return One of the nodes in the cycle or NULL if no cycle is present
 */
static inline header * detect_cycles() {
	for (int i = 0; i < N_LISTS; i++) {
		header * freelist = &freelistSentinels[i];
		for (header * slow = freelist->next, * fast = freelist->next->next; 
				fast != freelist; 
				slow = slow->next, fast = fast->next->next) {
			if (slow == fast) {
				return slow;
			}
		}
	}
	return NULL;
}

/**
 * @brief Helper to verify that there are no unlinked previous or next pointers
 *        in the free list
 *
 * @return A node whose previous and next pointers are incorrect or NULL if no
 *         such node exists
 */
static inline header * verify_pointers() {
	for (int i = 0; i < N_LISTS; i++) {
		header * freelist = &freelistSentinels[i];
		for (header * cur = freelist->next; cur != freelist; cur = cur->next) {
			if (cur->next->prev != cur || cur->prev->next != cur) {
				return cur;
			}
		}
	}
	return NULL;
}

/**
 *
 @brief Verify the structure of the free list is correct by checkin for 
 *        cycles and misdirected pointers
 *
 * @return true if the list is valid
 */
static inline bool verify_freelist() {
	header * cycle = detect_cycles();
	if (cycle != NULL) {
		fprintf(stderr, "Cycle Detected\n");
		print_sublist(print_object, cycle->next, cycle);
		return false;
	}

	header * invalid = verify_pointers();
	if (invalid != NULL) {
		fprintf(stderr, "Invalid pointers\n");
		print_object(invalid);
		return false;
	}

	return true;
}

/**
 * @brief Helper to verify that the sizes in a chunk from the OS are correct
 *        and that allocated node's canary values are correct
 *
 * @param chunk AREA_SIZE chunk allocated from the OS
 *
 * @return a pointer to an invalid header or NULL if all header's are valid
 */
static inline header * verify_chunk(header * chunk) {
	if (get_state(chunk) != FENCEPOST) {
		fprintf(stderr, "Invalid fencepost\n");
		print_object(chunk);
		return chunk;
	}

	for (; get_state(chunk) != FENCEPOST; chunk = get_right_header(chunk)) {
		if (get_size(chunk)  != get_right_header(chunk)->left_size) {
			fprintf(stderr, "Invalid sizes\n");
			print_object(chunk);
			return chunk;
		}
	}

	return NULL;
}

/**
 * @brief For each chunk allocated by the OS verify that the boundary tags
 *        are consistent
 *
 * @return true if the boundary tags are valid
 */
static inline bool verify_tags() {
	for (size_t i = 0; i < numOsChunks; i++) {
		header * invalid = verify_chunk(osChunkList[i]);
		if (invalid != NULL) {
			return invalid;
		}
	}

	return NULL;
}

/**
 * @brief Initialize mutex lock and prepare an initial chunk of memory for allocation
 */
static void init() {
	// Initialize mutex for thread safety
	pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
	// Manually set printf buffer so it won't call malloc when debugging the allocator
	setvbuf(stdout, NULL, _IONBF, 0);
#endif // DEBUG

	// Allocate the first chunk from the OS
	header * block = allocate_chunk(ARENA_SIZE);

	header * prevFencePost = get_header_from_offset(block, -ALLOC_HEADER_SIZE);
	insert_os_chunk(prevFencePost);

	lastFencePost = get_header_from_offset(block, get_size(block));

	// Set the base pointer to the beginning of the first fencepost in the first
	// chunk from the OS
	base = ((char *) block) - ALLOC_HEADER_SIZE; //sizeof(header);

	// Initialize freelist sentinels
	for (int i = 0; i < N_LISTS; i++) {
		header * freelist = &freelistSentinels[i];
		freelist->next = freelist;
		freelist->prev = freelist;
	}

	// Insert first chunk into the free list
	header * freelist = &freelistSentinels[N_LISTS - 1];
	freelist->next = block;
	freelist->prev = block;
	block->next = freelist;
	block->prev = freelist;
}

/* 
 * External interface
 */
void * my_malloc(size_t size) {
	pthread_mutex_lock(&mutex);
	header * hdr = allocate_object(size); 
	pthread_mutex_unlock(&mutex);
	return hdr;
}

void * my_calloc(size_t nmemb, size_t size) {
	return memset(my_malloc(size * nmemb), 0, size * nmemb);
}

void * my_realloc(void * ptr, size_t size) {
	void * mem = my_malloc(size);
	memcpy(mem, ptr, size);
	my_free(ptr);
	return mem; 
}

void my_free(void * p) {
	pthread_mutex_lock(&mutex);
	deallocate_object(p);
	pthread_mutex_unlock(&mutex);
}

bool verify() {
	return verify_freelist() && verify_tags();
}
