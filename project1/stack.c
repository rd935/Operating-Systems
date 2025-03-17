/*
* Add NetID and names of all project partners
* Siya Vyas - sv694
* Ritwika Das - rd935
* CS 416
* ilab1
*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void signal_handle(int signalno) {

    printf("OMG, I was slain!\n");

    int length_of_bad_instruction = sizeof(int);

    void *program_counter_location;
    asm("movl %%esp, %0" : "=r" (program_counter_location));
    int *program_counter = (int *)((char *) program_counter_location + sizeof(void *));

    *program_counter += length_of_bad_instruction;
}

int main(int argc, char *argv[]) {

    int x=5, y = 0, z=4;

    /* Step 1: Register signal handler first*/

    signal(SIGFPE, signal_handle);

    // This will generate floating point exception
    z=x/y;

    printf("LOL, I live again !!!%d\n", z);

    return 0;
}

