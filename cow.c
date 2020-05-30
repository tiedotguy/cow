#define _GNU_SOURCE
#define _POSIX_C_SOURCE 202005

#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <unistd.h>
#include <time.h>


static void *RoundDown(void *Base, int Align) {
  return Base - ((intptr_t)Base % Align);
}

struct cow_page {
  struct cow_page *Next;
  int Offset;
};

struct cow_block {
  struct cow_block *Next;
  size_t Size;
  char *Name;

  int OriginalFile;
  void *OriginalBaseAddress;
  void *ViewBaseAddress;

  int ShadowFile;
  void *ShadowBaseAddress;

  struct cow_page *DirtyPages;
};

struct cow_handler {
  struct sigaction OldSA;
  struct cow_block *Blocks;
};

struct cow_handler COWHandler;

void COWDumpBlock(struct cow_block *Block);
static int CreateFileMapping(char *Filename, size_t Size);
void *COWBlockPointer(struct cow_block *Block, void *Original);
void *COWPointer(struct cow_handler *Handler, void *Original);
struct cow_block *COWNewBlock(struct cow_handler *Handler, char *Name, size_t Size);
static void COWRemapPage(struct cow_block *Block, int Offset);
static void COWDemapPage(struct cow_block *Block, int Offset);
struct cow_block *COWFindBlock(struct cow_handler *Handler, void *Address);
static void COWHandleFault(struct cow_block *Block, void *Address);
static void COWSigSegv(struct cow_handler *Handler, int signal, siginfo_t *info);
void COWDisable(struct cow_block *Block);
void *COWEnable(struct cow_block *Block);

void COWDumpBlock(struct cow_block *Block) {
  printf("Size = %ld\n", Block->Size);
  printf("Name = %s\n", Block->Name);
  printf("OriginalFile = %d\n", Block->OriginalFile);
  printf("OriginalBaseAddress = %p\n", Block->OriginalBaseAddress);
  printf("ViewBaseAddress = %p\n", Block->ViewBaseAddress);
  printf("ShadowFile = %d\n", Block->ShadowFile);
  printf("ShadowBaseAddress = %p\n", Block->ShadowBaseAddress);
  printf("DirtyPages = \n");
  for (struct cow_page *Page = Block->DirtyPages;Page;Page=Page->Next) {
    printf("- %x\n", Page->Offset);
  }
}

static int CreateFileMapping(char *Filename, size_t Size) {
  int File = syscall(SYS_memfd_create, Filename, 0);
  if (File < 0) {
    return File;
  }

  int rc = ftruncate(File, Size);
  if (rc < 0) {
    close(File);
    return rc;
  }
  return File;
}


void *COWBlockPointer(struct cow_block *Block, void *Original) {
  if (Block->ShadowFile == -1) {
    return Original;
  }
  return (Original - Block->OriginalBaseAddress) + Block->ViewBaseAddress;
}

void *COWPointer(struct cow_handler *Handler, void *Original) {
  struct cow_block *Block = COWFindBlock(Handler, Original);
  assert(Block);
  return COWBlockPointer(Block, Original); 
}
    

struct cow_block *COWNewBlock(struct cow_handler *Handler, char *Name, size_t Size) {
  char *MappingName;
  struct cow_block *Block = malloc(sizeof(struct cow_block));
  Block->Name = strdup(Name);
  Block->Size = Size;
  Block->ShadowFile = -1;
  asprintf(&MappingName, "%s:original", Name);
  Block->OriginalFile = CreateFileMapping(MappingName, Size);
  free(MappingName);
  Block->OriginalBaseAddress = mmap(NULL, Size, PROT_READ | PROT_WRITE, MAP_SHARED, Block->OriginalFile, 0);
  Block->Next = Handler->Blocks;
  Handler->Blocks = Block;
  printf("Mapped %p to %p\n", Block->OriginalBaseAddress, Block->OriginalBaseAddress + Block->Size);
  return Block;
}

static void COWRemapPage(struct cow_block *Block, int Offset) {
  memcpy(Block->ShadowBaseAddress + Offset, Block->OriginalBaseAddress + Offset, getpagesize()); 
  munmap(Block->OriginalBaseAddress + Offset, getpagesize());
  void *NewPage = mmap(Block->OriginalBaseAddress + Offset, getpagesize(), PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, Block->ShadowFile, Offset);
  if (NewPage != Block->OriginalBaseAddress + Offset) {
    // Page was moved, this is fatal
    exit(1);
  }

  struct cow_page *COWPage = malloc(sizeof(struct cow_page));
  COWPage->Offset = Offset;
  COWPage->Next = Block->DirtyPages;
  Block->DirtyPages = COWPage;

  printf("Remapped %x\n", Offset);
}


static void COWDemapPage(struct cow_block *Block, int Offset) {
  munmap(Block->OriginalBaseAddress + Offset, getpagesize());
  void *NewPage = mmap(Block->OriginalBaseAddress + Offset, getpagesize(), PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, Block->OriginalFile, Offset);
  if (NewPage != Block->OriginalBaseAddress + Offset) {
    // Page was moved, this is fatal
    exit(1);
  }
  memcpy(NewPage, Block->ShadowBaseAddress + Offset, getpagesize());
  printf("Demapped %x\n", Offset);
}

struct cow_block *COWFindBlock(struct cow_handler *Handler, void *Address) {
  for (struct cow_block *Block = Handler->Blocks;Block;Block = Block->Next) {
    if (Address >= Block->OriginalBaseAddress && Address < (Block->OriginalBaseAddress + Block->Size)) {
      return Block;
    }
  }
  return NULL;
}

static void COWHandleFault(struct cow_block *Block, void *Address) {
  assert(Block->ShadowFile != -1);
  void *Page = RoundDown(Address, getpagesize());
  COWRemapPage(Block, Page - Block->OriginalBaseAddress);
}

static void COWSigSegv(struct cow_handler *Handler, int signal, siginfo_t *info) {
  assert(signal == SIGSEGV);
  assert(info->si_code == SEGV_ACCERR);

  struct cow_block *Block = COWFindBlock(Handler, info->si_addr);
  // TODO: Invoke the behavior from Handler->OldSA if possible.
  assert(Block);

  COWHandleFault(Block, info->si_addr);
}

void *COWEnable(struct cow_block *Block) {
  assert(Block->ShadowFile == -1);

  char *Name;
  asprintf(&Name, "%s:copy", Block->Name);
  Block->ShadowFile = CreateFileMapping(Name, Block->Size);
  free(Name);
  Block->ShadowBaseAddress = mmap(NULL, Block->Size, PROT_READ | PROT_WRITE, MAP_SHARED, Block->ShadowFile, 0);
  if (Block->ShadowBaseAddress == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }

  Block->ViewBaseAddress = mmap(NULL, Block->Size, PROT_READ, MAP_SHARED, Block->OriginalFile, 0);

  mprotect(Block->OriginalBaseAddress, Block->Size, PROT_READ);

  return Block->ViewBaseAddress;
}

void COWDisable(struct cow_block *Block) {
  assert(Block->ShadowFile != -1);

  struct cow_page *NextDirtyPage;
  for (struct cow_page *DirtyPage = Block->DirtyPages;DirtyPage;DirtyPage = NextDirtyPage) {
    COWDemapPage(Block, DirtyPage->Offset);
    NextDirtyPage = DirtyPage->Next;
    free(DirtyPage);
  }
  Block->DirtyPages = NULL;
  munmap(Block->ShadowBaseAddress, Block->Size);
  Block->ShadowBaseAddress = NULL;
  close(Block->ShadowFile);
  Block->ShadowFile = -1;

  // Remap the whole file to consolidate regions.  This may not be necessary, it
  // depends if the OS will re-consolidate neighboring regions in mprotect or not.
  //if (munmap(Block->OriginalBaseAddress, Block->Size) < 0) {
    //perror("munmap");
    //exit(1);
  //}
  //void *Address = mmap(Block->OriginalBaseAddress, Block->Size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, Block->OriginalFile, 0);
  //assert(Address == Block->OriginalBaseAddress);
  munmap(Block->ViewBaseAddress, Block->Size);
  Block->ViewBaseAddress = NULL;
}

static void sigsegv(int signal, siginfo_t *info, void *unused) {
  COWSigSegv(&COWHandler, signal, info);
}

void Save(char *Filename, void *Buffer, int BufferSize, int Delay) {
  FILE *f = fopen(Filename, "wb");
  struct timespec ts;
  for (int i=0;i<BufferSize;i+=4096) {
    fwrite(Buffer + i, 4096, 1, f);
    if (Delay) {
      ts.tv_sec = 0;
      ts.tv_nsec = 1000;
      //nanosleep(&ts, NULL);
    }
  }
  printf("Done saving\n");
  fclose(f);
}

void COWInitialize(struct cow_handler *Handler) {
  struct sigaction SA = {0};
  sigset_t mask = {0};

  SA.sa_handler = NULL;
  SA.sa_sigaction = sigsegv;
  SA.sa_mask = mask;
  SA.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &SA, &Handler->OldSA);

  Handler->Blocks = NULL;
}

int main(int argc, char **argv) {
  char *Mem;
  int MapSize = 1048576;

  COWInitialize(&COWHandler);
  struct cow_block *Block2 = COWNewBlock(&COWHandler, "block2", MapSize);
  struct cow_block *Block1 = COWNewBlock(&COWHandler, "block1", MapSize);

  Mem = Block1->OriginalBaseAddress;
  for (int i=0;i<MapSize;i++) {
    Mem[i] = i & 0xff;
  }
  Save("save-original", Mem, MapSize, 0);

  void *View = COWEnable(Block1);

  Save("save-mem-after-enable", Mem, MapSize, 0);
  Save("save-view-after-enable", View, MapSize, 0);

  for (int i=0;i<100;i++) {
    Mem[rand() % MapSize] = 1;
  }
  sleep(1);

/*
  for (int i=0;i<MapSize;i++) {
    //struct timespec ts;
    //ts.tv_sec = 0;
    //ts.tv_nsec = 1000;
    //nanosleep(&ts, NULL);
    Mem[i] = 1;
    if ((i % getpagesize()) == 0) { printf("sleeping\n"); sleep(1); }
  }
*/

  Save("save-mem-after-change", Mem, MapSize, 0);
  Save("save-view-after-change", View, MapSize, 0);

  COWDisable(Block1);

  Save("save-mem-after-disable", Mem, MapSize, 0);
  COWDumpBlock(Block1);
  return 0;
}
