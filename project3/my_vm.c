#include <err.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include "my_vm.h"
#include <stdint.h>

//TODO: Define static variables and structs, include headers, etc.
#define ADDR_BITS 32

#define log_2(num) (log(num)/log(2))

#define top(x, num) ((x) >> (32 - num))
#define mid(x, num_mid, num_lower) ((x >> (num_lower)) & ((1UL << (num_mid)) - 1))
#define low(x, num) (((1UL << num) - 1) & (x))

#define set(map, index) \
    (((char *) map)[(index) / 8] |= (1UL << ((index) % 8)))

#define clear(map, index) \
    (((char *) map)[(index) / 8] &= ~(1UL << ((index) % 8)))

#define get(map, index) \
    (((char *) map)[(index) / 8] & (1UL << ((index) % 8)))

static struct tlb store;

static unsigned long mem_size;
static void *phys_mem;
static pde_t *page_dir;

static unsigned int off_bits;
static unsigned int dir_bits;
static unsigned int table_bits;
static unsigned int page_bits;

static unsigned long total_pages;
static unsigned char *alloc;
static unsigned char *virt;

//general lock
static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;

//locks for maps and tables
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

//for check TLB
static unsigned int misses;
static unsigned int lookups;

//allocating and setting physical memory
static void set_physical_mem(){
    //TODO: Finish
    unsigned long map_size;

    mem_size = MEMSIZE < MAX_MEMSIZE ? MEMSIZE : MAX_MEMSIZE;
    phys_mem = mmap(NULL, mem_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (phys_mem == MAP_FAILED){
        err(-1, "%s: Error allocating %lu bytes for physical memory", __func__, mem_size);
    }

    page_dir = phys_mem;
    off_bits = (unsigned int) log_2(PGSIZE);
    table_bits = ADDR_BITS - table_bits - off_bits;
    if(!dir_bits){
        unsigned int bits_remaining = ADDR_BITS - off_bits;
        table_bits = bits_remaining/2;
        if (bits_remaining % 2){
            table_bits++;
        }
        dir_bits = bits_remaining/2;
    }

    page_bits = table_bits + dir_bits;
    total_pages = mem_size/PGSIZE;

    map_size = total_pages/8;
    alloc = calloc(1, map_size);
    virt = calloc(1, map_size);
    if(!alloc || !virt){
        err(-1, "%s: Error allocating %lu bytes for bitmap", __func__, map_size);
    }

    set(alloc, 0);
    set(virt, 0);

    if(atexit(&free_mem) != 0){
        warn("%s: Setup of automatic freeing failed. Physical memory will not be freed at program exit", __func__);
    }    
}

//takes virtual address and page directory and performs translation
//returns physical address
static pte_t *translate(pde_t *pgdir, void *vpage){
    //TODO: Finish
    pte_t table_entry, *page_table;
	pde_t dir_entry;
	unsigned long dir_index, offset, table_index, table_num, page_num, addr;
	unsigned long vaddr = (unsigned long) vpage;

	if (!pgdir || !vpage){
		return NULL;
    }

	dir_index = top(vaddr, dir_bits);
	table_index = mid(vaddr, table_bits, off_bits);
	offset = low(vaddr, off_bits);
	page_num = (unsigned long) check_TLB(vpage);

	if (page_num){
		goto tlb_hit;
    }

	dir_entry = pgdir[dir_index];

	if (!dir_entry){
		return NULL;
    }

	table_num = low(dir_entry, page_bits);

	page_table = (pte_t *) ((char *) phys_mem + table_num * PGSIZE);
	table_entry = page_table[table_index];

	if (!table_entry){
		return NULL;
    }

	page_num = low(table_entry, page_bits);

tlb_hit:
	addr = (unsigned long) ((char *) phys_mem + page_num * PGSIZE);
	addr += offset;
	return (pte_t *) addr;
}

//takes page directory, virtual address and physical address and set page table entry
static int page_map(pde_t *pgdir, void *vpage, void *ppage){
    //TODO: Finish
    pte_t table_entry, *page_table;
    pde_t dir_entry;
    unsigned long dir_index, table_index, table_num, page_num, vaddr;

    if(!pgdir || !vpage || !ppage){
        return -1;
    }

    vaddr = (unsigned long) vpage;
    dir_index = top(vaddr, dir_bits);
    table_index = mid(vaddr, table_bits, off_bits);

    dir_entry = pgdir[dir_index];

    if(!dir_entry){
        dir_entry = alloc_table();
        set(alloc, dir_entry);
        pgdir[dir_index] = dir_entry;
    }

    table_num = low(dir_entry, page_bits);

    page_table = (pte_t *) ((char *) phys_mem + table_num*PGSIZE);
    table_entry = page_table[table_index];
    page_num = ((pte_t) ppage - (pte_t) phys_mem)/PGSIZE;

    if(table_entry != (pte_t) page_num){
        page_table[table_index] = (pte_t) page_num;
    }

    add_TLB(vpage, (void *) page_num);

    return 0;
}

//responsible for allocating pages and uses the benchmark
void *t_malloc(int n){
    //TODO: Finish
    static int init;
    void *vpage = NULL;
    unsigned int num_pages, i, open_entry, first_page;

    if(!n){
        return NULL;
    }

    pthread_mutex_lock(&mut);

    if (!init) {
        set_physical_mem();
        init = 1;
    }

    pthread_mutex_unlock(&mut);
    num_pages = n/PGSIZE;

    if(n % PGSIZE) {
        num_pages++;
    }

    pthread_mutex_lock(&map_lock);
    open_entry = get_next(num_pages, virt);

    if(!open_entry){
        goto err_unlock_map;
    }

    for(i = 0; i < num_pages; i++){
        set(virt, open_entry + i);
    }

    vpage = create_vaddr(open_entry++);
    first_page = get_next(1, alloc);

    if(!first_page){
        goto err_unlock_map;
    }

    set(alloc, first_page);
    page_map(page_dir, vpage, (char *) phys_mem + first_page*PGSIZE);
    pthread_mutex_unlock(&map_lock);

    for(i = 1; i < num_pages; i++, open_entry++){
        unsigned int ppn;
        void *vaddr_entry = create_vaddr(open_entry);

        pthread_mutex_lock(&map_lock);
        ppn = get_next(1, alloc);
        set(alloc, ppn);
        pthread_mutex_unlock(&map_lock);

        pthread_mutex_lock(&table_lock);
        page_map(page_dir, vaddr_entry, (char *) phys_mem + ppn*PGSIZE);
        pthread_mutex_unlock(&table_lock);
    }

    return vpage;

err_unlock_map:
    pthread_mutex_unlock(&map_lock);
    return NULL;

}

//responsible for releasing one or more memory pages using virtual address
void t_free(void *vpage, int n){
    //TODO: Finish
    unsigned long i, num_to_free, virt_map_index, vaddr;

    if(!vpage || n <= 0){
        return;
    }

    num_to_free = n/PGSIZE;
    
    if(n % PGSIZE){
        num_to_free++;
    }

    virt_map_index = vaddr_index(vpage);
    vaddr = (unsigned long) vpage;

    pthread_mutex_lock(&map_lock);
    for(i = 0; i < num_to_free; i++){
        if(get(virt, virt_map_index + i)) {
            pthread_mutex_unlock(&map_lock);
            return;
        }
    }
    pthread_mutex_unlock(&map_lock);

    for (i = 0; i < num_to_free; i++){
        unsigned long paddr, ppn;

        paddr = (unsigned long) translate(page_dir, (void *) vaddr);
        ppn = (paddr - (unsigned long) phys_mem)/PGSIZE;
        pthread_mutex_lock(&map_lock);
        clear(alloc, ppn);
        clear(virt, virt_map_index);
        pthread_mutex_unlock(&map_lock);

        invalidate_entry((void *) vaddr);

        virt_map_index++;
        vaddr += PGSIZE;
    }
}

//copies data pointed by val to physical memory using the virtual address
void put_value(void *vpage, void *val, int n){
    //TODO: Finish
    int i;
    char *paddr, *val_ptr = val, *vaddr = vpage;

    if(!vpage || !val || n <= 0){
        return;
    }

    for (i = 0; i < n; i++, vaddr++){
        paddr = (char *) translate(page_dir, (void *) vaddr);
        if(!paddr) {
            printf("%s: Address translation failed!\n", __func__);
            return;
        }

        *paddr = *val_ptr++;
    }
}

//given a virtual address, the function copies bytes from the page into val
void get_value(void *vpage, void *dst, int n){
    //TODO: Finish
    int i;
    char *paddr, *val_ptr = dst, *vaddr = vpage;

    if(!vpage || !dst || n <= 0){
        return;
    }

    for (i = 0; i < n; i++, vaddr++){
        paddr = (char *) translate(page_dir, vaddr);
        if (!paddr){
            printf("%s: Address translation failed!\n", __func__);
            return;
        }

        *val_ptr++ = *paddr;
    }
}

//recieves two matrices with size n, and performs the matrix multiplication 
void mat_mult(void *mat1, void *mat2, int n, void *ans){
    //TODO: Finish
    int i, k, j, num1, num2, total;
    unsigned int addr_mat1, addr_mat2, addr_ans;

    if(!mat1 || !mat2 || !ans || n <= 0) {
        return;
    }

    for (i = 0; i < n; i++){
        for (j = 0; j < n; j++){
            total = 0;
            for (k = 0; k < n; k++){
		uintptr_t addr_mat1 = (uintptr_t) mat1 + (i*n*sizeof(int)) + (k*sizeof(int));
                uintptr_t addr_mat2 = (uintptr_t) mat2 + (k*n*sizeof(int)) + (j*sizeof(int));

                get_value((void *) addr_mat1, &num1, sizeof(int));
                get_value((void *) addr_mat2, &num2, sizeof(int));
                total += num1*num2;
            }

	    uintptr_t addr_ans = (uintptr_t) ans + (i * n * sizeof(int)) + (j * sizeof(int));
            put_value((void *) addr_ans, &total, sizeof(int));

        }
    }
}

//add virtual to physical page to the TLB
static int add_TLB(void *vpage, void *ppage){
    //TODO: Finish
    unsigned long i, tag;

    tag = (unsigned long) vpage >> off_bits;
    i = tag % TLB_ENTRIES;
    store.entries[i].vaddr = tag;
    store.entries[i].p_num = (unsigned long) ppage;
    store.entries[i].valid = true;
    
    return 0;
}

//checl for valid translation
static pte_t *check_TLB(void *vpage){
    //TODO: Finish
    unsigned long i, tag;

    tag = (unsigned long) vpage >> off_bits;
    i = tag % TLB_ENTRIES;

    lookups++;
    if(store.entries[i].valid && store.entries[i].vaddr == tag){
        return ((pte_t *) store.entries[i].p_num);
    }

    misses++;
    return NULL;
}

//print TLB miss rate
void print_TLB_missrate(){
    //TODO: Finish
    double miss_rate;

    miss_rate = lookups ? (double) misses/lookups : 0.0;
    fprintf(stderr, "TLB miss rate %f\n", miss_rate);
}

//cleans up physical memory mapping and bitmaps when finished
static void free_mem(void){
    munmap(phys_mem, mem_size);
    free(alloc);
    free(virt);
}

//allocates a new table
static pde_t alloc_table(void){
	unsigned long i;

	for (i = 0; i < total_pages; i++) {
		if (!get(alloc, i)) {
			set(alloc, i);
			return i;
		}
	}

	return 0;
}

//returns the next available entries from bitmap
static unsigned int get_next(int n, unsigned char *map){
	unsigned int i, free_page = 0, available_pages = 0;

	for (i = 0; i < total_pages; i++) {
		if (!get(map, i)) {
			if (!free_page){
				free_page = i;
            }

			available_pages++;

			if (available_pages == n){
				return free_page;
            }

		} 
        
        else {
			free_page = 0;
			available_pages = 0;
		}
	}
	return 0;
}

//creates virtual address
static void *create_vaddr(unsigned long ppn){
	unsigned long new_vaddr, dir_entries, table_entries;

	dir_entries = 1 << dir_bits;
	table_entries = 1 << table_bits;
	new_vaddr = (ppn / dir_entries) << table_bits;
	new_vaddr |= ppn % table_entries;
	new_vaddr <<= off_bits;
	return (void *) new_vaddr;
}

//converts virtual address to the index in the bitmap
static unsigned int vaddr_index(void *vpage){
	unsigned long vaddr = (unsigned long) vpage;
	unsigned int index;

	index = top(vaddr, dir_bits) << table_bits;
	index += mid(vaddr, table_bits, off_bits);
	return index;
}

//invalidates a virtual address in TLB
static void invalidate_entry(void *vpage){
	unsigned long i, tag;

	tag = (unsigned long) vpage >> off_bits;
	i = tag % TLB_ENTRIES;

	if (store.entries[i].valid && store.entries[i].vaddr == tag)
		store.entries[i].valid = false;
}
