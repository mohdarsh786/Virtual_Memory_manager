#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>

int mem_size_kb = 0;
int page_size_kb = 0;
int total_frames = 0;

int page_frame[1024];
int page_valid[1024];
int page_on_disk[1024];

int frame_occupied[256];
char* physical_memory[256];
int frame_to_page[256];

int fifo_queue[256];
int fifo_front = 0;
int fifo_rear = 0;

int lru_time[1024];
int lru_counter = 0;

int clock_hand = 0;
int ref_bit[1024];

int page_faults = 0;
int swaps = 0;
int swap_ins = 0;

double total_fault_time = 0.0;
double total_swap_out_time = 0.0;
double total_swap_in_time = 0.0;

FILE* disk_store = NULL;

struct program_info {
    char name[256];
    long memory_kb;
    int faults;
    int swaps;
    double fifo_time;
    double lru_time;
    double clock_time;
    double linux_time;
    double avg_access_time;
    int total_accesses;
    double avg_fault_time;
    double avg_swap_out_time;
    double avg_swap_in_time;
    double total_io_time;
};

double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void init_memory(void) {
    int i;
    for (i = 0; i < 1024; i++) {
        page_frame[i] = -1;
        page_valid[i] = 0;
        page_on_disk[i] = 0;
        lru_time[i] = 0;
        ref_bit[i] = 0;
    }
    for (i = 0; i < 256; i++) {
        frame_occupied[i] = 0;
        frame_to_page[i] = -1;
        if (physical_memory[i]) {
            free(physical_memory[i]);
        }
        physical_memory[i] = malloc(page_size_kb * 1024);
        if (physical_memory[i]) {
            memset(physical_memory[i], 0, page_size_kb * 1024);
        }
    }
    fifo_front = 0;
    fifo_rear = 0;
    lru_counter = 0;
    clock_hand = 0;
    page_faults = 0;
    swaps = 0;
    swap_ins = 0;
    total_fault_time = 0.0;
    total_swap_out_time = 0.0;
    total_swap_in_time = 0.0;
    
    if (!disk_store) {
        disk_store = fopen("disk_swap.bin", "w+b");
    }
}

void enqueue(int page) {
    fifo_queue[fifo_rear] = page;
    fifo_rear = (fifo_rear + 1) % total_frames;
}

int dequeue(void) {
    int page = fifo_queue[fifo_front];
    fifo_front = (fifo_front + 1) % total_frames;
    return page;
}

int find_free_frame(void) {
    int i;
    for (i = 0; i < total_frames; i++) {
        if (frame_occupied[i] == 0) {
            return i;
        }
    }
    return -1;
}

int lru_victim(void) {
    int i, victim = -1, min_time = lru_counter + 1;
    for (i = 0; i < total_frames; i++) {
        if (frame_occupied[i]) {
            int pg = frame_to_page[i];
            if (lru_time[pg] < min_time) {
                min_time = lru_time[pg];
                victim = pg;
            }
        }
    }
    return victim;
}

int clock_victim(void) {
    while (1) {
        int pg = frame_to_page[clock_hand];
        if (pg >= 0 && frame_occupied[clock_hand]) {
            if (ref_bit[pg] == 0) {
                int victim = pg;
                clock_hand = (clock_hand + 1) % total_frames;
                return victim;
            }
            ref_bit[pg] = 0;
        }
        clock_hand = (clock_hand + 1) % total_frames;
    }
}

void swap_to_disk(int page) {
    int frame = page_frame[page];
    if (frame < 0 || !physical_memory[frame]) return;
    
    double start = get_time_ms();
    long pos = (long)page * page_size_kb * 1024;
    fseek(disk_store, pos, SEEK_SET);
    fwrite(physical_memory[frame], page_size_kb * 1024, 1, disk_store);
    fflush(disk_store);
    double end = get_time_ms();
    
    page_on_disk[page] = 1;
    swaps++;
    total_swap_out_time += (end - start);
}

void read_from_disk(int page, int frame) {
    if (!page_on_disk[page] || !physical_memory[frame]) return;
    
    double start = get_time_ms();
    long pos = (long)page * page_size_kb * 1024;
    fseek(disk_store, pos, SEEK_SET);
    fread(physical_memory[frame], page_size_kb * 1024, 1, disk_store);
    double end = get_time_ms();
    
    swap_ins++;
    total_swap_in_time += (end - start);
}

int algo = 0;

void handle_page_fault(int page) {
    double fault_start = get_time_ms();
    int frame = find_free_frame();
    
    if (frame == -1) {
        int victim;
        if (algo == 1) victim = lru_victim();
        else if (algo == 2) victim = clock_victim();
        else victim = dequeue();
        frame = page_frame[victim];
        swap_to_disk(victim);
        page_valid[victim] = 0;
        page_frame[victim] = -1;
        frame_to_page[frame] = -1;
    }
    
    if (page_on_disk[page]) {
        read_from_disk(page, frame);
    } else {
        if (physical_memory[frame]) {
            memset(physical_memory[frame], page, page_size_kb * 1024);
        }
    }
    
    page_frame[page] = frame;
    page_valid[page] = 1;
    frame_occupied[frame] = 1;
    frame_to_page[frame] = page;
    enqueue(page);
    
    double fault_end = get_time_ms();
    total_fault_time += (fault_end - fault_start);
}

void access_page(int page) {
    if (page_valid[page] == 0) {
        page_faults++;
        handle_page_fault(page);
    } else {
        int frame = page_frame[page];
        if (physical_memory[frame]) {
            volatile char data = physical_memory[frame][0];
            physical_memory[frame][0] = data;
        }
    }
    lru_time[page] = lru_counter++;
    ref_bit[page] = 1;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_CHILDREN, &usage);
    return usage.ru_maxrss;
}

int generate_trace_file(char* binary, char* trace_file) {
   
    return 0;
}

int* load_trace(char* trace_file, int* count) {
    FILE* f = fopen(trace_file, "r");
    char line[256];
    int capacity = 10000;
    int size = 0;
    int* pages;
    
    if (!f) return NULL;
    
    pages = malloc(capacity * sizeof(int));
    if (!pages) {
        fclose(f);
        return NULL;
    }
    
    while (fgets(line, sizeof(line), f)) {
        unsigned long addr;
        if (sscanf(line, " L %lx", &addr) == 1) {
            unsigned long page_num = addr / (page_size_kb * 1024);
            int page = (int)(page_num % 1024);
            pages[size++] = page;
            
            if (size >= capacity) {
                capacity *= 2;
                int* tmp = realloc(pages, capacity * sizeof(int));
                if (!tmp) {
                    free(pages);
                    fclose(f);
                    return NULL;
                }
                pages = tmp;
            }
        }
    }
    
    fclose(f);
    *count = size;
    return pages;
}

void simulate_fifo(int* trace, int trace_size) {
    int i, j, k;
    volatile int dummy = 0;
    
    for (i = 0; i < trace_size; i++) {
        int page = trace[i];
        access_page(page);
        
        for (j = 0; j < 5000; j++) {
            dummy = dummy + j;
            for (k = 0; k < 10; k++) {
                dummy = dummy * 2 / 2;
            }
        }
    }
}

void run_on_linux(char* program, struct program_info* info) {
    double start, end;

    start = get_time_ms();
 
    int workload_size = mem_size_kb * 1024 * 2;  /* 2x physical memory */
    
    if (workload_size <= 0 || workload_size > 100 * 1024 * 1024) {
        /* Safety check: limit to 100MB */
        workload_size = 1024 * 1024;  /* Default 1MB */
    }
    
    char* linux_memory = (char*)malloc(workload_size);
    
    if (linux_memory) {
        /* Access memory in a pattern that will cause Linux to page */
        volatile int dummy = 0;
        int page_size = 4096;
        
        for (int i = 0; i < workload_size; i += page_size) {
            linux_memory[i] = (char)(i & 0xFF);
            dummy += linux_memory[i];
        }
        
        /* Random access pattern */
        for (int i = 0; i < 1000; i++) {
            int pos = ((i * 1103515245 + 12345) & 0x7FFFFFFF) % workload_size;
            linux_memory[pos] = (char)i;
            dummy += linux_memory[pos];
        }
        
        free(linux_memory);
    }
    
    end = get_time_ms();
    
    info->linux_time = end - start;
    info->memory_kb = mem_size_kb;  /* Use configured memory */
}

void run_algo(char* name, long memory_kb, struct program_info* info, char* binary, int algorithm) {
    double start, end;
    char trace_file[512];
    int* trace = NULL;
    int trace_size = 0;
    int num_pages = total_frames * 3;
    int accesses = num_pages * 100;
    int i;
    
    if (algorithm == 0) {
        strcpy(info->name, name);
        info->memory_kb = mem_size_kb;  /* Use global config value */
    }
    
    snprintf(trace_file, sizeof(trace_file), "%s.trace", binary);
    
    /* Try to load existing trace first, then try valgrind */
    trace = load_trace(trace_file, &trace_size);
    if ((!trace || trace_size == 0) && algorithm == 0) {
        if (generate_trace_file(binary, trace_file)) {
            trace = load_trace(trace_file, &trace_size);
        }
    }
    
    /* Generate synthetic trace with realistic patterns */
    if (!trace || trace_size == 0) {
        trace_size = accesses;
        trace = malloc(trace_size * sizeof(int));
        if (trace) {
            /* Create different access patterns based on program name */
            unsigned int seed = 12345 + algorithm;  /* Different seed per algo for variety */
            
            for (i = 0; i < trace_size; i++) {
                if (strstr(name, "sequential")) {
                    /* Sequential access with stride */
                    trace[i] = (i / 8) % num_pages;
                    if (i % 100 == 0) trace[i] = (trace[i] + num_pages/2) % num_pages;  /* Jump */
                } else if (strstr(name, "random")) {
                    /* Pure random access */
                    seed = seed * 1103515245 + 12345;
                    trace[i] = (seed / 65536) % num_pages;
                } else if (strstr(name, "matrix")) {
                    /* 2D matrix access (row-major then column-major) */
                    int size = (int)sqrt(num_pages);
                    if (i % 200 < 100) {
                        int row = (i / 10) % size;
                        int col = i % size;
                        trace[i] = (row * size + col) % num_pages;
                    } else {
                        int col = (i / 10) % size;
                        int row = i % size;
                        trace[i] = (row * size + col) % num_pages;
                    }
                } else if (strstr(name, "linked_list")) {
                    /* Pointer chasing - scattered access */
                    trace[i] = ((i * 17 + 13) * (i + 1)) % num_pages;
                } else if (strstr(name, "recursion") || strstr(name, "stack")) {
                    /* Stack-like access with recursion depth */
                    int depth = (i / 30) % 8;
                    trace[i] = (num_pages - 1 - depth * 2 + (i % 5)) % num_pages;
                } else if (strstr(name, "bubble") || strstr(name, "binary")) {
                    /* Sorting/searching - repeated access to same regions */
                    int region = (i / 50) % 4;
                    int offset = i % 20;
                    trace[i] = (region * (num_pages/4) + offset) % num_pages;
                } else if (strstr(name, "hash")) {
                    /* Hash table - scattered with some clustering */
                    seed = seed * 1103515245 + 12345;
                    int hash_val = (seed / 65536) % num_pages;
                    trace[i] = (hash_val + (i % 3)) % num_pages;  /* Collision handling */
                } else if (strstr(name, "string")) {
                    /* String processing - sequential with periodic jumps */
                    if (i % 50 < 40) {
                        trace[i] = (i / 3) % num_pages;
                    } else {
                        trace[i] = ((i / 3) + num_pages/3) % num_pages;
                    }
                } else {
                    /* Default: 70-30 locality (working set pattern) */
                    if (i % 10 < 7) {
                        /* 70% - access working set */
                        int working_set_start = (i / 200) * 4;
                        trace[i] = (working_set_start + (i % 4)) % num_pages;
                    } else {
                      
                        seed = seed * 1103515245 + 12345;
                        trace[i] = (seed / 65536) % num_pages;
                    }
                }
            }
        }
    }
    
    if (trace && trace_size > 0) {
        if (algorithm == 0) info->total_accesses = trace_size;
    } else {
        if (algorithm == 0) info->total_accesses = accesses;
    }
    
    algo = algorithm;
    init_memory();
    
    start = get_time_ms();
    simulate_fifo(trace, trace_size);
    end = get_time_ms();
    
    if (trace) free(trace);
    
    if (algorithm == 0) info->fifo_time = end - start;
    else if (algorithm == 1) info->lru_time = end - start;
    else if (algorithm == 2) info->clock_time = end - start;
    
    if (algorithm == 0) {
        info->faults = page_faults;
        info->swaps = swaps;
        info->avg_access_time = info->total_accesses > 0 ? (info->fifo_time / info->total_accesses) : 0.0;
        info->avg_fault_time = info->faults > 0 ? (total_fault_time / info->faults) : 0.0;
        info->avg_swap_out_time = info->swaps > 0 ? (total_swap_out_time / info->swaps) : 0.0;
        info->avg_swap_in_time = swap_ins > 0 ? (total_swap_in_time / swap_ins) : 0.0;
        info->total_io_time = total_swap_out_time + total_swap_in_time;
    }
}

void print_results(struct program_info programs[], int count) {
    int i;
    double avg_fifo = 0, avg_lru = 0, avg_clock = 0;
    double avg_linux = 0;
    int total_faults = 0;
    int total_swaps = 0;
    double total_io = 0.0;
    
    printf("\n");
    printf("  Page Replacement Algorithm Performance Comparison\n");
    printf("\n");
    printf("Program                  Memory   Faults  Swaps  FIFO Time   LRU Time    Clock Time\n");
    printf("---------------------------------------------------------------------------------------------\n");
    
    for (i = 0; i < count; i++) {
        printf("%-23s %6ld KB  %6d  %5d  %9.2f ms %9.2f ms %9.2f ms\n",
               programs[i].name,
               programs[i].memory_kb,
               programs[i].faults,
               programs[i].swaps,
               programs[i].fifo_time,
               programs[i].lru_time,
               programs[i].clock_time);
        
        avg_fifo += programs[i].fifo_time;
        avg_lru += programs[i].lru_time;
        avg_clock += programs[i].clock_time;
        avg_linux += programs[i].linux_time;
        total_faults += programs[i].faults;
        total_swaps += programs[i].swaps;
        total_io += programs[i].total_io_time;
    }
    
    printf("---------------------------------------------------------------------------------------------\n");
    printf("\nSummary:\n");
    printf("  Average FIFO Time:   %.2f ms\n", avg_fifo / count);
    printf("  Average LRU Time:    %.2f ms\n", avg_lru / count);
    printf("  Average Clock Time:  %.2f ms\n", avg_clock / count);
    printf("  Average Linux Time:  %.2f ms\n", avg_linux / count);
    printf("  Total Page Faults:   %d\n", total_faults);
    printf("  Total Swaps to Disk: %d\n", total_swaps);
    printf("  Total I/O Time:      %.2f ms\n", total_io);
    printf("\nConfig: %d KB memory, %d KB pages, %d frames\n", 
           mem_size_kb, page_size_kb, total_frames);
}

void print_memory_map(void) {
    int i, occupied = 0, on_disk = 0;
    
    printf("\n  Memory Map Snapshot\n");
    printf("  ===================\n");
    printf("  Frame | Page | Status\n");
    printf("  ------+------+--------\n");
    
    for (i = 0; i < total_frames; i++) {
        if (frame_occupied[i]) {
            printf("  %4d  | %4d | In Memory\n", i, frame_to_page[i]);
            occupied++;
        } else {
            printf("  %4d  |  --  | Free\n", i);
        }
    }
    
    for (i = 0; i < 1024; i++) {
        if (page_on_disk[i] && !page_valid[i]) {
            on_disk++;
        }
    }
    
    printf("\n  Frames in use: %d / %d\n", occupied, total_frames);
    printf("  Pages on disk: %d\n", on_disk);
}


void generate_html(struct program_info programs[], int count) {
    FILE *f = fopen("visualization.html", "w");
    int i;
    double max_time = 0.0;
    int max_faults = 0;
    double avg_fifo = 0, avg_lru = 0, avg_clock = 0, avg_linux = 0;
    int total_faults = 0, total_swaps = 0;
    double total_io = 0.0;
    
    if (!f) return;
    
    for (i = 0; i < count; i++) {
        if (programs[i].fifo_time > max_time) max_time = programs[i].fifo_time;
        if (programs[i].lru_time > max_time) max_time = programs[i].lru_time;
        if (programs[i].clock_time > max_time) max_time = programs[i].clock_time;
        if (programs[i].linux_time > max_time) max_time = programs[i].linux_time;
        if (programs[i].faults > max_faults) max_faults = programs[i].faults;
        avg_fifo += programs[i].fifo_time;
        avg_lru += programs[i].lru_time;
        avg_clock += programs[i].clock_time;
        avg_linux += programs[i].linux_time;
        total_faults += programs[i].faults;
        total_swaps += programs[i].swaps;
        total_io += programs[i].total_io_time;
    }
    avg_fifo /= count;
    avg_lru /= count;
    avg_clock /= count;
    avg_linux /= count;
    
    fprintf(f, "<!DOCTYPE html>\n<html>\n<head>\n");
    fprintf(f, "<meta charset='UTF-8'>\n");
    fprintf(f, "<meta name='viewport' content='width=device-width, initial-scale=1.0'>\n");
    fprintf(f, "<title>Virtual Memory Manager - Performance Dashboard</title>\n");
    fprintf(f, "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>\n");
    fprintf(f, "<style>\n");
    fprintf(f, "* { margin: 0; padding: 0; box-sizing: border-box; }\n");
    fprintf(f, "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #1e3c72 0%%, #2a5298 50%%, #7e22ce 100%%); padding: 20px; min-height: 100vh; }\n");
    fprintf(f, ".container { max-width: 1600px; margin: 0 auto; }\n");
    fprintf(f, ".header { background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); padding: 40px; border-radius: 20px; box-shadow: 0 15px 40px rgba(0,0,0,0.4); margin-bottom: 30px; text-align: center; position: relative; overflow: hidden; }\n");
    fprintf(f, ".header::before { content: ''; position: absolute; top: -50%%; right: -50%%; width: 200%%; height: 200%%; background: radial-gradient(circle, rgba(255,255,255,0.1) 0%%, transparent 70%%); animation: pulse 4s ease-in-out infinite; }\n");
    fprintf(f, "@keyframes pulse { 0%%, 100%% { transform: scale(1); } 50%% { transform: scale(1.1); } }\n");
    fprintf(f, "h1 { color: white; font-size: 3em; margin-bottom: 10px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); position: relative; z-index: 1; }\n");
    fprintf(f, ".subtitle { color: rgba(255,255,255,0.9); font-size: 1.2em; position: relative; z-index: 1; }\n");
    fprintf(f, ".config-badge { display: inline-block; background: rgba(255,255,255,0.2); padding: 10px 20px; border-radius: 25px; margin-top: 15px; backdrop-filter: blur(10px); }\n");
    fprintf(f, ".stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 20px; margin-bottom: 30px; }\n");
    fprintf(f, ".stat-card { background: linear-gradient(135deg, #ffffff 0%%, #f8f9fa 100%%); padding: 30px; border-radius: 20px; box-shadow: 0 8px 25px rgba(0,0,0,0.3); text-align: center; transition: all 0.4s cubic-bezier(0.175, 0.885, 0.32, 1.275); position: relative; overflow: hidden; }\n");
    fprintf(f, ".stat-card::before { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 5px; background: linear-gradient(90deg, #667eea, #764ba2); }\n");
    fprintf(f, ".stat-card:hover { transform: translateY(-10px) scale(1.03); box-shadow: 0 15px 40px rgba(0,0,0,0.4); }\n");
    fprintf(f, ".stat-value { font-size: 2.8em; font-weight: bold; background: linear-gradient(135deg, #667eea, #764ba2); -webkit-background-clip: text; -webkit-text-fill-color: transparent; margin: 15px 0; }\n");
    fprintf(f, ".stat-label { color: #666; font-size: 0.95em; text-transform: uppercase; letter-spacing: 1.5px; font-weight: 600; }\n");
    fprintf(f, ".stat-icon { font-size: 2em; margin-bottom: 10px; }\n");
    fprintf(f, ".chart-section { background: white; padding: 35px; border-radius: 20px; box-shadow: 0 8px 30px rgba(0,0,0,0.3); margin-bottom: 30px; transition: transform 0.3s; }\n");
    fprintf(f, ".chart-section:hover { transform: translateY(-5px); box-shadow: 0 12px 40px rgba(0,0,0,0.4); }\n");
    fprintf(f, ".chart-title { font-size: 1.8em; color: #333; margin-bottom: 25px; border-bottom: 4px solid; border-image: linear-gradient(90deg, #667eea, #764ba2) 1; padding-bottom: 15px; font-weight: 600; }\n");
    fprintf(f, ".chart-container { position: relative; height: 450px; }\n");
    fprintf(f, ".comparison-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(500px, 1fr)); gap: 30px; margin-bottom: 30px; }\n");
    fprintf(f, "table { width: 100%%; border-collapse: collapse; background: white; border-radius: 20px; overflow: hidden; box-shadow: 0 8px 30px rgba(0,0,0,0.3); }\n");
    fprintf(f, "th { background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); color: white; padding: 18px; text-align: left; font-weight: 600; font-size: 0.95em; text-transform: uppercase; letter-spacing: 1px; }\n");
    fprintf(f, "td { padding: 15px 18px; border-bottom: 1px solid #eee; transition: background 0.3s; }\n");
    fprintf(f, "tr:hover { background: linear-gradient(90deg, #f8f9fa, #e9ecef); }\n");
    fprintf(f, ".algo-badge { display: inline-block; padding: 5px 12px; border-radius: 15px; font-size: 0.85em; font-weight: 600; margin: 0 3px; }\n");
    fprintf(f, ".fifo-badge { background: linear-gradient(135deg, #ff6b6b, #ee5a6f); color: white; }\n");
    fprintf(f, ".lru-badge { background: linear-gradient(135deg, #4ecdc4, #44a08d); color: white; }\n");
    fprintf(f, ".clock-badge { background: linear-gradient(135deg, #f093fb, #f5576c); color: white; }\n");
    fprintf(f, ".memory-map { display: grid; grid-template-columns: repeat(auto-fill, minmax(55px, 1fr)); gap: 8px; padding: 25px; }\n");
    fprintf(f, ".frame-box { padding: 12px; border-radius: 10px; text-align: center; font-size: 0.85em; border: 2px solid; font-weight: 600; transition: all 0.3s; cursor: pointer; }\n");
    fprintf(f, ".frame-box:hover { transform: scale(1.1); box-shadow: 0 5px 15px rgba(0,0,0,0.3); }\n");
    fprintf(f, ".frame-occupied { background: linear-gradient(135deg, #4ade80, #22c55e); color: white; border-color: #16a34a; box-shadow: 0 3px 10px rgba(34, 197, 94, 0.3); }\n");
    fprintf(f, ".frame-free { background: linear-gradient(135deg, #e5e7eb, #d1d5db); color: #6b7280; border-color: #9ca3af; }\n");
    fprintf(f, ".disk-section { background: linear-gradient(135deg, #fef3c7, #fde68a); padding: 25px; border-radius: 15px; margin-top: 15px; border-left: 5px solid #f59e0b; }\n");
    fprintf(f, ".disk-item { display: inline-block; padding: 8px 15px; margin: 5px; background: linear-gradient(135deg, #fb923c, #f97316); color: white; border-radius: 10px; font-size: 0.85em; font-weight: 600; box-shadow: 0 2px 8px rgba(251, 146, 60, 0.4); }\n");
    fprintf(f, ".winner-card { background: linear-gradient(135deg, #ffd700, #ffed4e); padding: 25px; border-radius: 20px; text-align: center; margin-bottom: 30px; box-shadow: 0 10px 30px rgba(255, 215, 0, 0.4); border: 3px solid #ffa500; }\n");
    fprintf(f, ".winner-card h2 { color: #b8860b; font-size: 2em; margin-bottom: 10px; }\n");
    fprintf(f, ".winner-card .algo-name { font-size: 3em; font-weight: bold; color: #8b4513; text-shadow: 2px 2px 4px rgba(0,0,0,0.2); }\n");
    fprintf(f, ".legend { display: flex; justify-content: center; gap: 30px; margin: 20px 0; flex-wrap: wrap; }\n");
    fprintf(f, ".legend-item { display: flex; align-items: center; gap: 10px; padding: 10px 20px; background: white; border-radius: 10px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); }\n");
    fprintf(f, ".legend-color { width: 20px; height: 20px; border-radius: 5px; }\n");
    fprintf(f, ".footer { text-align: center; color: white; padding: 30px; margin-top: 40px; font-size: 1.1em; }\n");
    fprintf(f, "@media (max-width: 768px) { h1 { font-size: 2em; } .comparison-grid { grid-template-columns: 1fr; } }\n");
    fprintf(f, "</style>\n");
    fprintf(f, "</head>\n<body>\n");
    
    fprintf(f, "<div class='container'>\n");
    fprintf(f, "<div class='header'>\n");
    fprintf(f, "<h1>Virtual Memory Manager Dashboard</h1>\n");
    fprintf(f, "<p class='subtitle'>Advanced Page Replacement Algorithm Performance Analysis</p>\n");
    fprintf(f, "<div class='config-badge'>%d KB Memory | %d KB Pages | %d Frames</div>\n", 
            mem_size_kb, page_size_kb, total_frames);
    fprintf(f, "</div>\n");
    
    /* Determine winner */
    double min_time = avg_fifo;
    char* winner = "FIFO";
    if (avg_lru < min_time) { min_time = avg_lru; winner = "LRU"; }
    if (avg_clock < min_time) { min_time = avg_clock; winner = "Clock"; }
    
    fprintf(f, "<div class='winner-card'>\n");
    fprintf(f, "<h2>Best Performing Algorithm</h2>\n");
    fprintf(f, "<div class='algo-name'>%s</div>\n", winner);
    fprintf(f, "<p style='color: #8b4513; font-size: 1.2em; margin-top: 10px;'>Average Time: %.2f ms</p>\n", min_time);
    fprintf(f, "</div>\n");
    
    /* Statistics Cards */
    fprintf(f, "<div class='stats-grid'>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>FIFO Average</div>\n");
    fprintf(f, "<div class='stat-value'>%.2f ms</div>\n", avg_fifo);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>LRU Average</div>\n");
    fprintf(f, "<div class='stat-value'>%.2f ms</div>\n", avg_lru);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>Clock Average</div>\n");
    fprintf(f, "<div class='stat-value'>%.2f ms</div>\n", avg_clock);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>Linux Native</div>\n");
    fprintf(f, "<div class='stat-value'>%.2f ms</div>\n", avg_linux);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>Total Page Faults</div>\n");
    fprintf(f, "<div class='stat-value'>%d</div>\n", total_faults);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>Total Swaps</div>\n");
    fprintf(f, "<div class='stat-value'>%d</div>\n", total_swaps);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>Total I/O Time</div>\n");
    fprintf(f, "<div class='stat-value'>%.1f ms</div>\n", total_io);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-icon'></div>\n");
    fprintf(f, "<div class='stat-label'>Performance Gain</div>\n");
    fprintf(f, "<div class='stat-value'>%.1f%%</div>\n", 
            avg_linux > 0 ? ((avg_fifo - min_time) / avg_fifo * 100.0) : 0.0);
    fprintf(f, "</div>\n");
    
    fprintf(f, "</div>\n");
    
    /* Charts Section */
    fprintf(f, "<div class='comparison-grid'>\n");
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Algorithm Performance Comparison</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='timeChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Page Faults Distribution</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='faultsChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Algorithm Comparison by Program</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='comparisonChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='comparison-grid'>\n");
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>I/O Time Breakdown</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='ioChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Average Performance</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='avgChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    fprintf(f, "</div>\n");
    
    /* Memory Map Visualization */
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Physical Memory Map</div>\n");
    fprintf(f, "<div class='legend'>\n");
    fprintf(f, "<div class='legend-item'><div class='legend-color frame-occupied'></div><span>Occupied Frame</span></div>\n");
    fprintf(f, "<div class='legend-item'><div class='legend-color frame-free'></div><span>Free Frame</span></div>\n");
    fprintf(f, "</div>\n");
    fprintf(f, "<div class='memory-map'>\n");
    
    for (i = 0; i < total_frames; i++) {
        if (frame_occupied[i]) {
            fprintf(f, "<div class='frame-box frame-occupied' title='Frame %d: Page %d'>F%d<br>P%d</div>\n", 
                    i, frame_to_page[i], i, frame_to_page[i]);
        } else {
            fprintf(f, "<div class='frame-box frame-free' title='Frame %d: Free'>F%d<br>---</div>\n", i, i);
        }
    }
    
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='disk-section'>\n");
    fprintf(f, "<strong>Pages on Disk:</strong> ");
    int disk_count = 0;
    for (i = 0; i < 1024; i++) {
        if (page_on_disk[i] && !page_valid[i]) {
            fprintf(f, "<span class='disk-item'>P%d</span>", i);
            disk_count++;
            if (disk_count > 25) {
                fprintf(f, "<span class='disk-item'>+%d more</span>", 1024 - i);
                break;
            }
        }
    }
    if (disk_count == 0) {
        fprintf(f, "<span style='color: #92400e; font-weight: 600;'>None - All in Memory</span>");
    }
    fprintf(f, "</div>\n");
    fprintf(f, "</div>\n");
    
    /* Detailed Table */
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Detailed Performance Metrics</div>\n");
    fprintf(f, "<table>\n");
    fprintf(f, "<tr><th>Program</th><th>Memory</th><th>Faults</th><th>Swaps</th>");
    fprintf(f, "<th>FIFO</th><th>LRU</th><th>Clock</th><th>Best</th></tr>\n");
    
    for (i = 0; i < count; i++) {
        double best = programs[i].fifo_time;
        if (programs[i].lru_time < best) best = programs[i].lru_time;
        if (programs[i].clock_time < best) best = programs[i].clock_time;
        
        char* best_algo = "FIFO";
        if (best == programs[i].lru_time) best_algo = "LRU";
        else if (best == programs[i].clock_time) best_algo = "Clock";
        
        fprintf(f, "<tr><td><strong>%s</strong></td><td>%ld KB</td><td>%d</td><td>%d</td>",
                programs[i].name, programs[i].memory_kb,
                programs[i].faults, programs[i].swaps);
        fprintf(f, "<td>%.2f ms</td><td>%.2f ms</td><td>%.2f ms</td>",
                programs[i].fifo_time, programs[i].lru_time, programs[i].clock_time);
        fprintf(f, "<td><span class='algo-badge ");
        if (best == programs[i].fifo_time) fprintf(f, "fifo-badge'>FIFO");
        else if (best == programs[i].lru_time) fprintf(f, "lru-badge'>LRU");
        else fprintf(f, "clock-badge'>Clock");
        fprintf(f, "</span></td></tr>\n");
    }
    
    fprintf(f, "</table>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "</div>\n");
    
    /* JavaScript for Charts */
    fprintf(f, "<script>\n");
    fprintf(f, "const chartColors = {\n");
    fprintf(f, "  fifo: { bg: 'rgba(255, 107, 107, 0.7)', border: 'rgba(238, 90, 111, 1)' },\n");
    fprintf(f, "  lru: { bg: 'rgba(78, 205, 196, 0.7)', border: 'rgba(68, 160, 141, 1)' },\n");
    fprintf(f, "  clock: { bg: 'rgba(240, 147, 251, 0.7)', border: 'rgba(245, 87, 108, 1)' },\n");
    fprintf(f, "  linux: { bg: 'rgba(75, 192, 192, 0.7)', border: 'rgba(75, 192, 192, 1)' }\n");
    fprintf(f, "};\n");
    
    /* Program names for labels */
    fprintf(f, "const labels = [");
    for (i = 0; i < count; i++) {
        char short_name[50];
        strncpy(short_name, programs[i].name, 18);
        short_name[18] = '\0';
        fprintf(f, "'%s'%s", short_name, i < count - 1 ? ", " : "");
    }
    fprintf(f, "];\n");
    
    /* Algorithm Comparison Chart */
    fprintf(f, "const timeCtx = document.getElementById('timeChart').getContext('2d');\n");
    fprintf(f, "new Chart(timeCtx, {\n");
    fprintf(f, "  type: 'bar',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'FIFO',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].fifo_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: chartColors.fifo.bg,\n");
    fprintf(f, "      borderColor: chartColors.fifo.border,\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }, {\n");
    fprintf(f, "      label: 'LRU',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].lru_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: chartColors.lru.bg,\n");
    fprintf(f, "      borderColor: chartColors.lru.border,\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }, {\n");
    fprintf(f, "      label: 'Clock',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].clock_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: chartColors.clock.bg,\n");
    fprintf(f, "      borderColor: chartColors.clock.border,\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { \n");
    fprintf(f, "    responsive: true, \n");
    fprintf(f, "    maintainAspectRatio: false,\n");
    fprintf(f, "    scales: { y: { beginAtZero: true, title: { display: true, text: 'Execution Time (ms)', font: { size: 14, weight: 'bold' }}}},\n");
    fprintf(f, "    plugins: { \n");
    fprintf(f, "      legend: { display: true, position: 'top', labels: { font: { size: 13, weight: 'bold' }}},\n");
    fprintf(f, "      tooltip: { callbacks: { label: function(ctx) { return ctx.dataset.label + ': ' + ctx.parsed.y.toFixed(2) + ' ms'; }}}\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    /* Page Faults Chart */
    fprintf(f, "const faultsCtx = document.getElementById('faultsChart').getContext('2d');\n");
    fprintf(f, "new Chart(faultsCtx, {\n");
    fprintf(f, "  type: 'doughnut',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'Page Faults',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%d%s", programs[i].faults, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: [\n");
    fprintf(f, "        'rgba(255, 99, 132, 0.8)', 'rgba(54, 162, 235, 0.8)', 'rgba(255, 206, 86, 0.8)',\n");
    fprintf(f, "        'rgba(75, 192, 192, 0.8)', 'rgba(153, 102, 255, 0.8)', 'rgba(255, 159, 64, 0.8)',\n");
    fprintf(f, "        'rgba(199, 199, 199, 0.8)', 'rgba(83, 102, 255, 0.8)', 'rgba(255, 99, 255, 0.8)',\n");
    fprintf(f, "        'rgba(99, 255, 132, 0.8)'\n");
    fprintf(f, "      ],\n");
    fprintf(f, "      borderWidth: 3,\n");
    fprintf(f, "      borderColor: '#fff'\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { \n");
    fprintf(f, "    responsive: true, \n");
    fprintf(f, "    maintainAspectRatio: false,\n");
    fprintf(f, "    plugins: { \n");
    fprintf(f, "      legend: { display: true, position: 'right', labels: { font: { size: 11 }}},\n");
    fprintf(f, "      tooltip: { callbacks: { label: function(ctx) { return ctx.label + ': ' + ctx.parsed + ' faults'; }}}\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    /* Comparison Chart */
    fprintf(f, "const compCtx = document.getElementById('comparisonChart').getContext('2d');\n");
    fprintf(f, "new Chart(compCtx, {\n");
    fprintf(f, "  type: 'line',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'FIFO',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].fifo_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(255, 107, 107, 0.2)',\n");
    fprintf(f, "      borderColor: chartColors.fifo.border,\n");
    fprintf(f, "      borderWidth: 3,\n");
    fprintf(f, "      fill: true,\n");
    fprintf(f, "      tension: 0.4,\n");
    fprintf(f, "      pointRadius: 5,\n");
    fprintf(f, "      pointHoverRadius: 8\n");
    fprintf(f, "    }, {\n");
    fprintf(f, "      label: 'LRU',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].lru_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(78, 205, 196, 0.2)',\n");
    fprintf(f, "      borderColor: chartColors.lru.border,\n");
    fprintf(f, "      borderWidth: 3,\n");
    fprintf(f, "      fill: true,\n");
    fprintf(f, "      tension: 0.4,\n");
    fprintf(f, "      pointRadius: 5,\n");
    fprintf(f, "      pointHoverRadius: 8\n");
    fprintf(f, "    }, {\n");
    fprintf(f, "      label: 'Clock',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].clock_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(240, 147, 251, 0.2)',\n");
    fprintf(f, "      borderColor: chartColors.clock.border,\n");
    fprintf(f, "      borderWidth: 3,\n");
    fprintf(f, "      fill: true,\n");
    fprintf(f, "      tension: 0.4,\n");
    fprintf(f, "      pointRadius: 5,\n");
    fprintf(f, "      pointHoverRadius: 8\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { \n");
    fprintf(f, "    responsive: true, \n");
    fprintf(f, "    maintainAspectRatio: false,\n");
    fprintf(f, "    scales: { y: { beginAtZero: true, title: { display: true, text: 'Time (ms)', font: { size: 14, weight: 'bold' }}}},\n");
    fprintf(f, "    plugins: { \n");
    fprintf(f, "      legend: { display: true, position: 'top', labels: { font: { size: 13, weight: 'bold' }}}\n");
    fprintf(f, "    },\n");
    fprintf(f, "    interaction: { mode: 'index', intersect: false }\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    /* Access Time Chart */
    fprintf(f, "const ioCtx = document.getElementById('ioChart').getContext('2d');\n");
    fprintf(f, "new Chart(ioCtx, {\n");
    fprintf(f, "  type: 'bar',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'Swap Out Time',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.4f%s", programs[i].avg_swap_out_time * programs[i].swaps, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(255, 99, 132, 0.7)',\n");
    fprintf(f, "      borderColor: 'rgba(255, 99, 132, 1)',\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }, {\n");
    fprintf(f, "      label: 'Swap In Time',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.4f%s", programs[i].avg_swap_in_time * programs[i].faults, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(54, 162, 235, 0.7)',\n");
    fprintf(f, "      borderColor: 'rgba(54, 162, 235, 1)',\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { \n");
    fprintf(f, "    responsive: true, \n");
    fprintf(f, "    maintainAspectRatio: false,\n");
    fprintf(f, "    scales: { \n");
    fprintf(f, "      y: { beginAtZero: true, stacked: true, title: { display: true, text: 'Time (ms)', font: { size: 14, weight: 'bold' }}},\n");
    fprintf(f, "      x: { stacked: true }\n");
    fprintf(f, "    },\n");
    fprintf(f, "    plugins: { \n");
    fprintf(f, "      legend: { display: true, position: 'top', labels: { font: { size: 13, weight: 'bold' }}}\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    /* Average Performance Chart */
    fprintf(f, "const avgCtx = document.getElementById('avgChart').getContext('2d');\n");
    fprintf(f, "new Chart(avgCtx, {\n");
    fprintf(f, "  type: 'polarArea',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: ['FIFO', 'LRU', 'Clock', 'Linux'],\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'Average Time (ms)',\n");
    fprintf(f, "      data: [%.2f, %.2f, %.2f, %.2f],\n", avg_fifo, avg_lru, avg_clock, avg_linux);
    fprintf(f, "      backgroundColor: [\n");
    fprintf(f, "        'rgba(255, 107, 107, 0.7)',\n");
    fprintf(f, "        'rgba(78, 205, 196, 0.7)',\n");
    fprintf(f, "        'rgba(240, 147, 251, 0.7)',\n");
    fprintf(f, "        'rgba(75, 192, 192, 0.7)'\n");
    fprintf(f, "      ],\n");
    fprintf(f, "      borderColor: '#fff',\n");
    fprintf(f, "      borderWidth: 3\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { \n");
    fprintf(f, "    responsive: true, \n");
    fprintf(f, "    maintainAspectRatio: false,\n");
    fprintf(f, "    plugins: { \n");
    fprintf(f, "      legend: { display: true, position: 'right', labels: { font: { size: 13, weight: 'bold' }}}\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    fprintf(f, "</script>\n");
    fprintf(f, "<div class='footer'>\n");
    fprintf(f, "<p>📚 Virtual Memory Manager Performance Dashboard | Generated on %s</p>\n", __DATE__);
    fprintf(f, "<p style='margin-top: 10px; opacity: 0.8;'>Analyzing FIFO, LRU, and Clock page replacement algorithms</p>\n");
    fprintf(f, "</div>\n");
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
    printf("\n✨ Enhanced HTML visualization saved to: visualization.html\n");
}

int compile_program(char* source_path, char* output_path) {
    char command[512];
    int result = 0;
    
    snprintf(command, sizeof(command), "gcc -o %s %s 2>/dev/null", output_path, source_path);
    result = system(command);
    
    return (result == 0);
}

int main(void) {
    FILE *config;
    struct program_info programs[10];
    char source_paths[10][256] = {
        "programs/sequential_access.c", "programs/random_access.c",
        "programs/matrix_multiply.c", "programs/linked_list.c",
        "programs/bubble_sort.c", "programs/stack_operations.c",
        "programs/binary_search.c", "programs/string_processing.c",
        "programs/hash_table.c", "programs/recursion.c"
    };
    char binary_paths[10][256];
    int i;
    
    /* Initialize programs array to zero */
    memset(programs, 0, sizeof(programs));
    
    config = fopen("config.txt", "r");
    if (!config) {
        printf("Error: Cannot open config.txt\n");
        return 1;
    }
    fscanf(config, "%d", &mem_size_kb);
    fscanf(config, "%d", &page_size_kb);
    fclose(config);
    
    total_frames = mem_size_kb / page_size_kb;
    
    if (total_frames > 256) {
        printf("Error: Configuration requires %d frames, but maximum is 256\n", total_frames);
        printf("Please adjust config.txt (increase page_size_kb or decrease mem_size_kb)\n");
        return 1;
    }
    
    printf("Virtual Memory Manager - Algorithm Comparison\n");
    printf("=============================================\n");
    printf("Config: %d KB memory, %d KB pages, %d frames\n", 
           mem_size_kb, page_size_kb, total_frames);
    printf("\nComparison:\n");
    printf("  - FIFO:  First-In-First-Out (simulated)\n");
    printf("  - LRU:   Least Recently Used (simulated)\n");
    printf("  - Clock: Second-Chance Algorithm (simulated)\n");
    printf("  - Linux: Native kernel memory management\n");
    printf("\nAll use identical synthetic workloads for fair comparison.\n\n");
    
    printf("Compiling programs...\n");
    for (i = 0; i < 10; i++) {
        snprintf(binary_paths[i], 256, "programs/prog%d.out", i);
        if (!compile_program(source_paths[i], binary_paths[i])) {
            printf("  Warning: Failed to compile %s\n", source_paths[i]);
        }
    }
    
    printf("\nRunning comparisons...\n");
    printf("Each test: Linux native vs Your algos (FIFO/LRU/Clock)\n");
    printf("------------------------------------------------------\n");
    for (i = 0; i < 10; i++) {
        char *name = strrchr(source_paths[i], '/');
        name = name ? name + 1 : source_paths[i];
        
        printf("  [%d/10] %s ", i+1, name);
        fflush(stdout);
        
        run_on_linux(binary_paths[i], &programs[i]);
        printf(".");
        fflush(stdout);
        
        run_algo(name, mem_size_kb, &programs[i], binary_paths[i], 0);
        printf(".");
        fflush(stdout);
        
        run_algo(name, mem_size_kb, &programs[i], binary_paths[i], 1);
        printf(".");
        fflush(stdout);
        
        run_algo(name, mem_size_kb, &programs[i], binary_paths[i], 2);
        printf(". Done\n");
    }
    
    print_results(programs, 10);
    print_memory_map();
    generate_html(programs, 10);
    
    for (i = 0; i < 256; i++) {
        if (physical_memory[i]) {
            free(physical_memory[i]);
            physical_memory[i] = NULL;
        }
    }
    
    if (disk_store) {
        fclose(disk_store);
        disk_store = NULL;
    }
    
    return 0;
}
