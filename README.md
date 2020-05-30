cow
===

Basic proof-of-concept for copy-on-write memory allocating and handling.  This is intended for a game to be able to perform background saving without having to use immutable data structures.


Idea
----

On initialization, a signal handler is installed, which will handle page faults by remapping them from one region of memory to another.  This is a `cow_handler`.

A `cow_handler` creates and dispatches most work to a `cow_block`.  A `cow_block` is a single chunk of memory which is backed by 2 anonymous memory mapped files (`original` and `shadow`).  A `cow_block` is expected to own a large amount of memory which is then carved up further by something such as a slab allocator.  This isn't implemented in this PoC.

During typical usage, the memory in a `cow_block` is read-write.  When a snapshot of memory is requested, each `cow_block` is put in to COW mode, which will make the memory read-only, create a second anonymous memory mapped file (which is not initially populated), and return a second read-only view of the `original` memory mapped file at a **different** base address, the `view`.

Attempting to write to a read-only `cow_block` memory region will copy the contents of the page in the `original` file to the `shadow` file, and re-map the address to point to it.

Anything attempting to access the `view` must go through a translation layer, to map the pointer from `original` space to `view` space.  This can be done either through the `cow_handler` (slow), or asking the `cow_block` (fast).

When copy-on-write is disabled for a `cow_block`, `original is marked read-write, data is copied from `shadow` back to `original`, and both `shadow` and `view` are released.



Functions
---------

- COWInitialize - sets up the signal handler
- COWNewBlock - allocates and returns a new `cow_block` of a given size
- COWFindBlock - looks up a `cow_block` given a pointer in `original` space
- COWBlockPointer - maps a pointer from `original` space to `view` space, given a specific `cow_block`
- COWPointer - `COWFindBlock(COWBlockPointer)`, convenience function which should probably be avoided due to the overhead of finding the block
- COWEnable - puts a `cow_block` in to COW mode, allocating the backing resources required to manage its lifecycle.  Returns the base `view` pointer.
- COWDisable - takes a `cow_block` out of COW mode, copying all pages back from `shadow` to `original`, and releasing any resources.
- COWDumpBlock - dump a blocks data to stdout for debugging


Sharp edges
-----------

- I have no idea what happens if multiple mutation threads trigger a fault at the same time, some mutexes might be a good idea.
- There's currently no way to delete a block.
- Remapping a page is two distinct release/create operations.  In theory, one could fail if the operating system decides to use the memory during that window.  It may be possible to recover by falling back to a synchronous saving in such a situation, I haven't thought it through.
- There's no Windows port, although all the pieces exist (Vectored exception handling, CreateFileMapping, MapViewOfFileEx).  I just don't have a C dev environment setup for Windows.


gl;hf
-----

Good luck.  Have fun.
