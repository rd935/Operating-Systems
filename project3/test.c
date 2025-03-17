#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "my_vm.c"

#define MATRIX_SIZE 3
#define SMALL_CHUNK_SIZE 1024
#define BIG_CHUNK_SIZE (1ULL<<32)
#define NUM_THREADS 4

// Global variables
pthread_mutex_t tlb_lock = PTHREAD_MUTEX_INITIALIZER;

// Function to test t_malloc(), t_free(), put_value(), get_value(), and mat_mult()
void *test_functions() {
    // Allocate memory using t_malloc()
    void *memory_chunk = t_malloc(SMALL_CHUNK_SIZE);

    // Put and get values in/from the memory chunk
    int value = 42;
    put_value(memory_chunk, &value, sizeof(int));
    int retrieved_value;
    get_value(memory_chunk, &retrieved_value, sizeof(int));
    printf("Retrieved value: %d\n", retrieved_value);

    // Allocate matrices
    int *matrix1 = t_malloc(MATRIX_SIZE * MATRIX_SIZE * sizeof(int));
    int *matrix2 = t_malloc(MATRIX_SIZE * MATRIX_SIZE * sizeof(int));
    int *result_matrix = t_malloc(MATRIX_SIZE * MATRIX_SIZE * sizeof(int));

    // Fill matrices with random values
    for (int i = 0; i < MATRIX_SIZE * MATRIX_SIZE; ++i) {
        matrix1[i] = rand() % 10;
        matrix2[i] = rand() % 10;
    }

    // Perform matrix multiplication
    mat_mult(matrix1, matrix2, MATRIX_SIZE, result_matrix);

    // Free allocated memory
    t_free(memory_chunk, SMALL_CHUNK_SIZE);
    t_free(matrix1, MATRIX_SIZE * MATRIX_SIZE * sizeof(int));
    t_free(matrix2, MATRIX_SIZE * MATRIX_SIZE * sizeof(int));
    t_free(result_matrix, MATRIX_SIZE * MATRIX_SIZE * sizeof(int));

    return NULL;
}

// Function to perform multi-threaded testing
void multi_thread_test() {
    pthread_t threads[NUM_THREADS];
    int i;
    for (i = 0; i < NUM_THREADS; ++i) {
        pthread_create(&threads[i], NULL, test_functions, NULL);
    }

    // Join threads
    for (i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }
}


// Function to add translation to TLB
void add_translation_to_tlb(void *vpage, void *ppage) {
    pthread_mutex_lock(&tlb_lock);
    add_TLB(vpage, ppage);
    pthread_mutex_unlock(&tlb_lock);
}

int main() {
    // Set up physical memory
    set_physical_mem();

    // Initialize TLB
    init_tlb();

    // Test t_malloc(), t_free(), put_value(), get_value(), and mat_mult()
    multi_thread_test();

    // Print TLB miss rate
    print_TLB_missrate();

    return 0;
}
