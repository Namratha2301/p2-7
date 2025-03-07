extern void ctx_switch(void **old_sp, void* new_sp);
extern void ctx_start(void **old_sp, void* new_sp);

void terminal_write(const char *str, int len) {
    for (int i = 0; i < len; i++) {
        *(char*)(0x10000000UL) = str[i];
    }
}

#include <string.h>  // for strlen() and strcat()
#include <stdlib.h>  // for itoa() and malloc()
#include <stdarg.h>  // for va_start(), va_end() and va_arg()

// Thread status definitions
#define UNUSED    0
#define RUNNABLE  1
#define RUNNING   2
#define BLOCKED   3

// Format function for printf
void format_to_str(char* out, const char* fmt, va_list args) {
    for(out[0] = 0; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            strncat(out, fmt, 1);
        } else {
            fmt++;
            if (*fmt == 's') {
                strcat(out, va_arg(args, char*));
            } else if (*fmt == 'd') {
                itoa(va_arg(args, int), out + strlen(out), 10);
            }
        }
    }
}

// Simple printf implementation
int printf(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    format_to_str(buf, format, args);
    va_end(args);
    terminal_write(buf, strlen(buf));

    return 0;
}

// Memory allocation helper
extern char __heap_start, __heap_max;
static char* brk = &__heap_start;
char* _sbrk(int size) {
    if (brk + size > (char*)&__heap_max) {
        terminal_write("_sbrk: heap grows too large\r\n", 29);
        return NULL;
    }

    char* old_brk = brk;
    brk += size;
    return old_brk;
}

// Thread Control Block
struct thread {
    int id;               // Thread ID
    void* sp;             // Stack pointer
    int status;           // Thread status
    void (*entry)(void*); // Entry function
    void* arg;            // Argument for entry function
};

// Condition variable structure
struct cv {
    int waiting_threads[32];  // Array to track threads waiting on this CV
    int count;                // Number of waiting threads
};

// Maximum 32 threads as specified
struct thread TCB[32];
int current_idx;       // Current running thread index
int thread_count = 0;  // Track number of active threads

// Run queue for scheduler - simple circular array
int runqueue[32];      // Queue of thread IDs
int run_head = 0;      // Head of the queue (dequeue from here)
int run_tail = 0;      // Tail of the queue (enqueue here)

// Function to handle the case when no threads are runnable
void no_runnable_threads() {
    printf("No runnable thread\n\r");
    while(1) { 
        // Infinite loop 
    }
}

// Initialize the TCB data structures
void thread_init() {
    // Initialize all TCBs
    for (int i = 0; i < 32; i++) {
        TCB[i].id = i;
        TCB[i].sp = NULL;
        TCB[i].status = UNUSED;
        TCB[i].entry = NULL;
        TCB[i].arg = NULL;
    }
    
    // Initialize the main thread (thread 0)
    TCB[0].status = RUNNING;
    current_idx = 0;
    thread_count = 1;
    
    // Initialize the run queue
    for (int i = 0; i < 32; i++) {
        runqueue[i] = -1;
    }
    run_head = 0;
    run_tail = 0;
}

// Add a thread to the run queue
void runqueue_add(int thread_id) {
    // Add to the tail of the queue
    runqueue[run_tail] = thread_id;
    run_tail = (run_tail + 1) % 32;
}

// Remove a thread from the run queue
void runqueue_remove(int thread_id) {
    // Find the thread in the queue
    int i = run_head;
    while (i != run_tail) {
        if (runqueue[i] == thread_id) {
            // Remove by shifting elements
            int j = i;
            while (j != run_tail - 1) {
                runqueue[j] = runqueue[(j + 1) % 32];
                j = (j + 1) % 32;
            }
            runqueue[(run_tail - 1 + 32) % 32] = -1;
            run_tail = (run_tail - 1 + 32) % 32;
            break;
        }
        i = (i + 1) % 32;
    }
}

// Get the next runnable thread
int scheduler() {
    if (run_head == run_tail) {
        // Queue is empty
        return -1;
    }
    
    // Find a RUNNABLE thread
    int found_idx = -1;
    for (int i = run_head; i != run_tail; i = (i + 1) % 32) {
        if (TCB[runqueue[i]].status == RUNNABLE) {
            found_idx = runqueue[i];
            
            // Move this thread to the back of the queue (round-robin)
            runqueue_remove(found_idx);
            runqueue_add(found_idx);
            break;
        }
    }
    
    return found_idx;
}

// Create a new thread with a 16KB stack
void thread_create(void (*entry)(void *arg), void *arg) {
    // Find an unused TCB slot
    int new_idx = -1;
    for (int i = 0; i < 32; i++) {
        if (TCB[i].status == UNUSED) {
            new_idx = i;
            break;
        }
    }
    
    if (new_idx == -1) {
        printf("Error: No available TCB slot\n\r");
        return;
    }
    
    // Allocate stack for the new thread
    void* stack = malloc(16 * 1024);
    if (stack == NULL) {
        printf("Error: Failed to allocate stack memory\n\r");
        return;
    }
    
    // Initialize the TCB for the new thread
    TCB[new_idx].status = RUNNABLE;
    TCB[new_idx].entry = entry;
    TCB[new_idx].arg = arg;
    thread_count++;
    
    // Add the new thread to the scheduler
    runqueue_add(new_idx);
    
    // Start the thread execution
    ctx_start(&TCB[current_idx].sp, stack + 16 * 1024);
}

// Called by ctx_start to run a thread's entry function
void ctx_entry() {
    // Get the entry function and argument from the TCB
    void (*entry)(void*) = TCB[current_idx].entry;
    void* arg = TCB[current_idx].arg;
    
    // Execute the entry function
    entry(arg);
    
    // If the entry function returns, terminate the thread
    thread_exit();
}

// Yield to another thread (if any)
void thread_yield() {
    // Mark current thread as RUNNABLE
    TCB[current_idx].status = RUNNABLE;
    
    // Find the next thread to run
    int next_idx = scheduler();
    
    // If no thread is available or if we're yielding to ourselves, just return
    if (next_idx == -1 || next_idx == current_idx) {
        TCB[current_idx].status = RUNNING;
        return;
    }
    
    // Save the current thread index
    int prev_idx = current_idx;
    
    // Update current thread and its status
    current_idx = next_idx;
    TCB[current_idx].status = RUNNING;
    
    // Perform context switch
    ctx_switch(&TCB[prev_idx].sp, TCB[current_idx].sp);
}

// Terminate the current thread
void thread_exit() {
    // Mark current thread as UNUSED
    TCB[current_idx].status = UNUSED;
    thread_count--;
    
    // Remove the thread from the scheduler
    runqueue_remove(current_idx);
    
    // Find the next thread to run
    int next_idx = scheduler();
    
    // If no thread is available, enter an infinite loop
    if (next_idx == -1) {
        no_runnable_threads();
    }
    
    // Update current thread and its status
    int prev_idx = current_idx;
    current_idx = next_idx;
    TCB[current_idx].status = RUNNING;
    
    // Perform context switch
    ctx_switch(&TCB[prev_idx].sp, TCB[current_idx].sp);
}

// Initialize a condition variable
void cv_init(struct cv* condition) {
    condition->count = 0;
    for (int i = 0; i < 32; i++) {
        condition->waiting_threads[i] = -1;
    }
}

// Release a condition variable
void cv_release(struct cv* condition) {
    // Should only be called when no threads are waiting
    if (condition->count > 0) {
        printf("Warning: Releasing CV with waiting threads\n\r");
    }
    
    // Reset the condition variable
    condition->count = 0;
    for (int i = 0; i < 32; i++) {
        condition->waiting_threads[i] = -1;
    }
}

// Wait for a condition variable
void cv_wait(struct cv* condition) {
    // Add the current thread to the waiting list
    condition->waiting_threads[condition->count++] = current_idx;
    
    // Mark the current thread as BLOCKED
    TCB[current_idx].status = BLOCKED;
    
    // Find the next thread to run
    int next_idx = scheduler();
    
    // If no thread is available, enter an infinite loop
    if (next_idx == -1) {
        no_runnable_threads();
    }
    
    // Save the current thread index
    int prev_idx = current_idx;
    
    // Update current thread and its status
    current_idx = next_idx;
    TCB[current_idx].status = RUNNING;
    
    // Perform context switch
    ctx_switch(&TCB[prev_idx].sp, TCB[current_idx].sp);
}

// Signal a condition variable to wake up ONE blocked thread
void cv_signal(struct cv* condition) {
    // If no threads are waiting, just return
    if (condition->count == 0) {
        return;
    }
    
    // Get the first waiting thread
    int thread_id = condition->waiting_threads[0];
    
    // Shift remaining waiting threads
    for (int i = 0; i < condition->count - 1; i++) {
        condition->waiting_threads[i] = condition->waiting_threads[i + 1];
    }
    condition->count--;
    
    // Mark the thread as RUNNABLE and add to scheduler
    TCB[thread_id].status = RUNNABLE;
    runqueue_add(thread_id);
}

// Test function for simple thread testing
void test_thread(void* arg) {
    char* name = (char*)arg;
    for (int i = 0; i < 5; i++) {
        printf("%s is running, count = %d\n\r", name, i);
        thread_yield();
    }
}

// Test function for producer-consumer pattern
#define BUFFER_SIZE 3
void* buffer[BUFFER_SIZE];
int count = 0;
int head = 0, tail = 0;
struct cv nonempty, nonfull;

void producer(void* item) {
    char* name = (char*)item;
    for (int i = 0; i < 5; i++) {
        while (count == BUFFER_SIZE) {
            printf("%s: Buffer full, waiting...\n\r", name);
            cv_wait(&nonfull);
        }
        
        // Add item to buffer
        buffer[tail] = (void*)(long)i;
        tail = (tail + 1) % BUFFER_SIZE;
        count++;
        
        printf("%s: Produced item %d\n\r", name, i);
        cv_signal(&nonempty);
        thread_yield();
    }
    
    printf("%s: Done producing\n\r", name);
}

void consumer(void* arg) {
    char* name = (char*)arg;
    for (int i = 0; i < 10; i++) {
        while (count == 0) {
            printf("%s: Buffer empty, waiting...\n\r", name);
            cv_wait(&nonempty);
        }
        
        // Remove item from buffer
        void* item = buffer[head];
        head = (head + 1) % BUFFER_SIZE;
        count--;
        
        printf("%s: Consumed item %d\n\r", name, (int)(long)item);
        cv_signal(&nonfull);
        thread_yield();
    }
    
    printf("%s: Done consuming\n\r", name);
}

// Test function for condition variable with multiple waiters
void waiter(void* arg) {
    int id = (int)(long)arg;
    static int value = 0;
    static struct cv barrier;
    static int init_done = 0;
    
    // Initialize the CV (only once)
    if (!init_done) {
        cv_init(&barrier);
        init_done = 1;
    }
    
    printf("Waiter %d: waiting at barrier\n\r", id);
    
    // Last thread to arrive wakes up others
    value++;
    if (value == 3) {
        printf("Waiter %d: Last to arrive, waking others\n\r", id);
        cv_signal(&barrier);
        cv_signal(&barrier);
    } else {
        cv_wait(&barrier);
    }
    
    printf("Waiter %d: passed barrier\n\r", id);
}

// Main function with test cases
int main() {
    printf("EGOS-2000 Cooperative Threading Implementation\n\r");
    
    // Initialize the threading system
    thread_init();
    
    // Test Case 1: Simple threading
    printf("\n\rTest Case 1: Simple Threading\n\r");
    thread_create(test_thread, "Thread 1");
    thread_create(test_thread, "Thread 2");
    test_thread("Main Thread");
    
    // Test Case 2: Producer-Consumer
    printf("\n\rTest Case 2: Producer-Consumer\n\r");
    cv_init(&nonempty);
    cv_init(&nonfull);
    thread_create(producer, "Producer 1");
    thread_create(producer, "Producer 2");
    thread_create(consumer, "Consumer");
    
    // Test Case 3: Multiple waiters with barrier
    printf("\n\rTest Case 3: Barrier with Multiple Waiters\n\r");
    thread_create(waiter, (void*)1);
    thread_create(waiter, (void*)2);
    thread_create(waiter, (void*)3);
    
    // Main thread exits - scheduler will pick up the next thread
    thread_exit();
    
    // Should never reach here
    return 0;
}