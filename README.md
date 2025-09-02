## KoPoolIteratable
___
A fast data structure that reduces allocations/deallocations, provides stable pointers, and enables fast iterations. Implementation of a pool allocator, designed to allow fast iteration and maintain more sequential allocation to reduce cache misses and improve memory layouts, while keeping pointers stable. To allow for fast iterations, it jumps through empty ranges and maintains this skip node list in the memory pool itself. The sizeof of the element must be >= 16 bytes because `SkipNodeHead` and `SkipNodeTail` are stored per skip list node. Also, a bit set is maintained for each elements. For more see **Implementation** section.
___
## Benchmark

As `UnorderedSet` used [unordered_dense](https://github.com/martinus/unordered_dense)

#### Allocation
	[KoPool] Allocate:      0.000031ms
	[STDVector] Push:       0.000013ms
	[UnorderedSet] Insert:  0.000111ms

	We see that allocation close to `std::vector` push

#### Deallocations
	[KoPool] Deallocate:    0.000052ms
	[STDVector] Pop:        0.000013ms
	[UnorderedSet] Erase:   0.000181ms

	We see deallocation closer to `std::vector` pop. The deallocation version binary searches
	pointer in the exponential allocated blocks range - if we use for deallocation index, not pointer, it
	will be little bit faster, but in this case issue with jumps in memory - need to maintain a skip list

#### Iterations
	[KoPool] Iterate:       4.168833ms
	[STDVector] Iterate:    6.973767ms
	[UnorderedSet] Iterate: 4.038233ms

	With enabled vectorization:
	[KoPool] Iterate:       3.750200ms
	[UnorderedSet] Iterate: 3.688067ms

	We see iterations close to `unordered_dense`. In this test in `std::vector`, we store pointers
	to allocated data in `KoPoolIteratable` and before iterations, we shuffle `std::vector` to simulate
	situation when we allocate some data, then them push to `std::vector`, and remove this element using
	swap - pop back technique, which shuffles pointers inside `std::vector`, also the pointer address can't
	be sequential on the default allocation. In this example, `UnorderedSet`, so fast because we add a pointer
	sequentially in `UnorderedSet` and don't shuffle, the `UnorderedSet` stores data in `std::vector`,
	so it models the ideal case when memory is sequential, the best case for cache, and no jumps

#### Implementation

Used a strategy of exponential blocks - for the x64 system is 64 blocks, a sequence of blocks where block entry index is a power of two (sum($2^0$ + ... + $2^{63}$ ) == $2^{64}$ - 1). Inside the first block, we store 2 elements $2^0$ + 1. To search a vacant block used the msb/lsb intrinsic. Also, for each memory block, a bit set is stored which indicates is an element or a skip node list.

**Exponential Structure**
![Exponential Structure](image/ExponentialStructure.png)

Embedded a free list approach to search free ranges, and inside each skip list `SkipNodeHead` or `SkipNodeTail`. The free list pointer, which points to skip nodes, is unordered, but skip nodes is the range between `SkipNodeHead` and `SkipNodeTail`, ordered, and as a result, allocation is partially sequential. To indicate a one-size range or is skip node, a bit set is used.

**Skip List Structure**
![Skip List Structure](image/SkipNodeStructure.png)

Also, when an element is deallocated, track the last empty block, and if it has 2 empty blocks, deallocate the largest block to reduce memory consumption. Also, the element size must be >= 16 bytes because `SkipNodeHead` and `SkipNodeTail` of the skip node are 16 bytes. The pool doesn't uses templates, because designed to use dynamically without any type, probably, templates by type can improve performance in some cases.
