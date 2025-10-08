#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Simple Virtual Memory Manager
 * All code in a single file for presentation purposes.
 */

// Configuration constants
int mem_size_kb = 0;
int page_size_kb = 0;
int total_frames = 0;
int total_virtual_pages = 1024;

// Page table entries - one per virtual page
int page_frame[1024];           // Frame number for each page (-1 if invalid)
int page_valid[1024];           // 1 if in memory, 0 otherwise
int page_dirty[1024];           // 1 if modified, 0 otherwise
int page_on_disk[1024];         // 1 if saved to disk, 0 otherwise
int page_disk_offset[1024];     // Location in backing store

// Frame table - one per physical frame
int frame_occupied[256];        // 1 if frame is in use, 0 if free
int frame_page[256];            // Which virtual page is in this frame

int fifo_queue[256];            // Queue of page numbers
int fifo_front = 0;             // Front of queue
int fifo_rear = 0;              // Rear of queue
int fifo_size = 0;              // Current queue size

// Metrics
int total_accesses = 0;
int total_faults = 0;
int total_hits = 0;
int total_swaps = 0;

// Backing store
FILE* backing_store = NULL;
int next_disk_offset = 0;

/*
 * Initialize all data structures.
 */
void initialize_system(void) {
    int i = 0;
    
    // Initialize page table
    for (i = 0; i < total_virtual_pages; i++) {
        page_frame[i] = -1;
        page_valid[i] = 0;
        page_dirty[i] = 0;
        page_on_disk[i] = 0;
        page_disk_offset[i] = -1;
    }
    
    // Initialize frame table
    for (i = 0; i < total_frames; i++) {
        frame_occupied[i] = 0;
        frame_page[i] = -1;
    }
    
    // Initialize FIFO queue
    fifo_front = 0;
    fifo_rear = 0;
    fifo_size = 0;
    
    // Initialize metrics
    total_accesses = 0;
    total_faults = 0;
    total_hits = 0;
    total_swaps = 0;
    
    // Create backing store file
    backing_store = fopen("backing_store.bin", "w+b");
    next_disk_offset = 0;
}

/*
 * Add a page to the FIFO queue.
 */
void enqueue_page(int virtual_page) {
    fifo_queue[fifo_rear] = virtual_page;
    fifo_rear = (fifo_rear + 1) % 256;
    fifo_size = fifo_size + 1;
}

/*
 * Remove and return the oldest page from the FIFO queue.
 */
int dequeue_page(void) {
    int victim_page = fifo_queue[fifo_front];
    fifo_front = (fifo_front + 1) % 256;
    fifo_size = fifo_size - 1;
    return victim_page;
}

/*
 * Find a free physical frame.
 * Returns frame number or -1 if none available.
 */
int find_free_frame(void) {
    int i = 0;
    for (i = 0; i < total_frames; i++) {
        if (frame_occupied[i] == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Swap a page out to disk.
 */
void swap_out(int virtual_page) {
    int offset = 0;
    long position = 0;
    int data = virtual_page;
    int i = 0;
    
    // Reuse existing disk location or allocate new one
    if (page_on_disk[virtual_page] == 1) {
        offset = page_disk_offset[virtual_page];
    } else {
        offset = next_disk_offset;
        next_disk_offset = next_disk_offset + 1;
        page_disk_offset[virtual_page] = offset;
    }
    
    // Calculate file position
    position = (long)offset * page_size_kb * 1024;
    fseek(backing_store, position, SEEK_SET);
    
    // Write page data
    for (i = 0; i < (page_size_kb * 1024) / sizeof(int); i++) {
        fwrite(&data, sizeof(int), 1, backing_store);
    }
    fflush(backing_store);
    
    page_on_disk[virtual_page] = 1;
    total_swaps = total_swaps + 1;
}

/*
 * Swap a page in from disk.
 */
void swap_in(int virtual_page) {
    long position = 0;
    int data = 0;
    int i = 0;
    
    // Calculate file position
    position = (long)page_disk_offset[virtual_page] * page_size_kb * 1024;
    fseek(backing_store, position, SEEK_SET);
    
    // Read page data
    for (i = 0; i < (page_size_kb * 1024) / sizeof(int); i++) {
        fread(&data, sizeof(int), 1, backing_store);
    }
}

/*
 * Handle a page fault.
 */
void handle_page_fault(int virtual_page) {
    int frame = 0;
    int victim_page = 0;
    
    // Find a free frame
    frame = find_free_frame();
    
    // If no free frame, use FIFO to select victim
    if (frame == -1) {
        victim_page = dequeue_page();
        frame = page_frame[victim_page];
        
        // If victim is dirty, swap it out
        if (page_dirty[victim_page] == 1) {
            swap_out(victim_page);
        }
        
        // Invalidate victim page
        page_valid[victim_page] = 0;
        page_frame[victim_page] = -1;
    }
    
    // Load requested page
    if (page_on_disk[virtual_page] == 1) {
        swap_in(virtual_page);
    }
    
    // Update page table
    page_frame[virtual_page] = frame;
    page_valid[virtual_page] = 1;
    page_dirty[virtual_page] = 0;
    
    // Update frame table
    frame_occupied[frame] = 1;
    frame_page[frame] = virtual_page;
    
    // Add to FIFO queue
    enqueue_page(virtual_page);
}

/*
 * Process a memory access.
 */
void process_access(int virtual_page, char access_type) {
    total_accesses = total_accesses + 1;
    
    // Check if page is in memory
    if (page_valid[virtual_page] == 1) {
        // Page hit
        total_hits = total_hits + 1;
        
        // Set dirty bit if write
        if (access_type == 'W') {
            page_dirty[virtual_page] = 1;
        }
    } else {
        // Page fault
        total_faults = total_faults + 1;
        handle_page_fault(virtual_page);
        
        // Set dirty bit if write
        if (access_type == 'W') {
            page_dirty[virtual_page] = 1;
        }
    }
}

/*
 * Print performance metrics.
 */
void print_report(void) {
    double fault_rate = 0.0;
    
    if (total_accesses > 0) {
        fault_rate = ((double)total_faults / (double)total_accesses) * 100.0;
    }
    
    printf("\n");
    printf("========================================\n");
    printf("  Virtual Memory Simulation Report\n");
    printf("========================================\n");
    printf("\n");
    printf("Total Memory Accesses:  %d\n", total_accesses);
    printf("Page Faults:            %d\n", total_faults);
    printf("Page Hits:              %d\n", total_hits);
    printf("Page Fault Rate:        %.2f%%\n", fault_rate);
    printf("Number of Swaps:        %d\n", total_swaps);
    printf("\n");
    printf("========================================\n");
}

/*
 * Main program.
 */
int main(void) {
    FILE* config_file = NULL;
    FILE* trace_file = NULL;
    int virtual_page = 0;
    char access_type = ' ';
    
    // Read configuration from config.txt
    config_file = fopen("config.txt", "r");
    if (config_file == NULL) {
        printf("Error: Cannot open config.txt\n");
        return 1;
    }
    
    fscanf(config_file, "%d", &mem_size_kb);
    fscanf(config_file, "%d", &page_size_kb);
    fclose(config_file);
    
    // Calculate number of frames
    total_frames = mem_size_kb / page_size_kb;
    
    printf("Virtual Memory Manager Simulation\n");
    printf("Memory Size: %d KB\n", mem_size_kb);
    printf("Page Size: %d KB\n", page_size_kb);
    printf("Total Frames: %d\n", total_frames);
    printf("Trace File: traces/trace1.txt\n");
    printf("\n");
    
    // Initialize system
    initialize_system();
    
    // Open trace file
    trace_file = fopen("traces/trace1.txt", "r");
    if (trace_file == NULL) {
        printf("Error: Cannot open trace file\n");
        return 1;
    }
    
    // Process each memory access
    while (fscanf(trace_file, "%d %c", &virtual_page, &access_type) == 2) {
        process_access(virtual_page, access_type);
    }
    
    // Close trace file
    fclose(trace_file);
    
    // Print results
    print_report();
    
    // Clean up
    if (backing_store != NULL) {
        fclose(backing_store);
    }
    
    return 0;
}
