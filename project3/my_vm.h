#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#define PGSIZE 8192  // multiples of 8KB
#define MAX_MEMSIZE (1ULL<<32)
#define MEMSIZE (1ULL<<30)
#define TLB_ENTRIES 256

typedef unsigned long pte_t;
typedef unsigned long pde_t;

struct tlb {
    struct {
        unsigned long vaddr;
        unsigned long p_num;
        bool valid;
    } entries[TLB_ENTRIES];
};

static void set_physical_mem();

static pte_t * translate(pde_t *pgdir, void *vpage);

static int page_map(pde_t *pgdir, void *vpage, void *ppage);

void * t_malloc(int n);

void t_free(void *vpage, int n);

void put_value(void *vpage, void *val, int n);

void get_value(void *vpage, void *dst, int n);

void mat_mult(void *mat1, void *mat2, int n, void *ans);

static int add_TLB(void *vpage, void *ppage);

static pte_t *check_TLB(void *vpage);

void print_TLB_missrate();

static void invalidate_entry(void *vpage);

static void free_mem(void);

static pde_t alloc_table(void);

static unsigned int get_next(int n, unsigned char *bmap);

static void *create_vaddr(unsigned long ppn);

static unsigned int vaddr_index(void *vpage);
