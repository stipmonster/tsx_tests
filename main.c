#define _GNU_SOURCE
#include <immintrin.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <x86intrin.h>
#include <unistd.h>
#include <sys/types.h>


inline uint64_t timing() {
 asm volatile("xor %%rax, %%rax \n cpuid" ::: "rax", "rdx", "rbx", "rcx");

  uint64_t timing = __rdtsc();
  return timing;
}


static inline int flush_reload(void *ptr) {
  uint64_t start = 0, end = 0;

  start = timing();
  *(volatile char *)ptr;
  end = timing();

  _mm_clflush(ptr);
  return end - start;
}

uint64_t check_bit_13() {
  int i = 0x7;
  uint32_t regs[4];
  asm volatile("cpuid"
               : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
               : "a"(i), "c"(0));
  return (regs[3] >> 13) & 1;
}

uint64_t get_threshold() {
  int threshold = 1;
  int results[256] = {};

  int timing = 0;
  int timing_2 = 0;
  volatile char *buf = "AAAAAAAA";
  _mm_clflush((void *)buf);
  for (int i = 0; i < 20; i++) {
    timing += flush_reload((void *)buf);
    _mm_mfence();
    volatile char tmp = buf[0];
    _mm_mfence();
    timing_2 += flush_reload((void *)buf);
  }
  threshold = (timing + 2 * timing_2) / (20 * 3);
  fprintf(stderr, "[INFO] threshold %d\n", threshold);
  return threshold;
}

uint64_t read_msr(uint64_t msr_reg) {

  int core = sched_getcpu();
  uint64_t value = 0;
  fprintf(stderr, "[INFO] running on core %x reading %#lx\n", core,
          msr_reg);
  char buf[50];
  snprintf(buf, 50, "/dev/cpu/%d/msr", core);
  FILE *msr = fopen(buf, "r+");
  if (msr == NULL) {
    perror("msr");
    fprintf(stderr, "is the msr module loaded?\n");
    return -1;
  }
  fseek(msr, msr_reg, SEEK_SET);
  int ret = fread(&value, 1, sizeof(uint64_t), msr);
  fclose(msr);
  if(ret < 8)
	  return -1;
  return value;
}
void write_msr(uint64_t msr_reg, uint64_t value) {

  int core = sched_getcpu();
  fprintf(stderr, "[INFO] running on core %x setting %#lx to %#lx\n", core,
          msr_reg, value);
  char buf[50];
  snprintf(buf, 50, "/dev/cpu/%d/msr", core);
  FILE *msr = fopen(buf, "r+");
  if (msr == NULL) {
    perror("msr");
    fprintf(stderr, "is the msr module loaded?\n");
    return;
  }
  fseek(msr, msr_reg, SEEK_SET);
  int ret = fwrite(&value, 1, sizeof(uint64_t), msr);
  if (ret < 8) {
    fprintf(stderr, "write failed\n");
  }
  fclose(msr);
}

void do_tsx() {
  char volatile *buffer =
      mmap(NULL, sizeof(char) * 1024 * 256, PROT_READ | PROT_WRITE,
           MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  memset((char *)buffer, 0x42, 1024 * 256);

  _mm_mfence();
  int threshold = get_threshold();
  int results[256] = {};


  for (uint64_t i = 0; i < 256 * 1024; i++)
    _mm_clflush((void *)&buffer[i]);
  _mm_mfence();
  
  for (int i = 0; i < 1000; i++) {

    unsigned stat = _xbegin();
    if (stat == _XBEGIN_STARTED) {
      volatile char tmp_store = buffer[0x40 * 1024];
      //_xabort(0);
      _xend();
    }
    for (int m = 0; m < 256; ++m) {
      int j = ((m * 167) + 13) & 0xff;
      if (flush_reload((void *)(buffer + j * 1024)) < threshold) {
        results[j]++;
      }
    }
  }
  for (int i = 0; i < 256; i++)
    if (results[i] > 500)
      printf("%#x: %d\n", i, results[i]);

  munmap((void *)buffer, sizeof(char) * 1024 * 256);
}

int main(int argc, char **argv) {
  if (!check_bit_13()) {
    fprintf(stderr, "No support for TSX_FORCE_ABORT\n");
    return -1;
  }
  if (getuid() !=0) {
    fprintf(stderr, "root is required\n");
    return -1;
  }
  write_msr(0x10f, 0);
  do_tsx();
  write_msr(0x10f, 1);
  do_tsx();
  write_msr(0x10f, 0);
  return 0;
}
