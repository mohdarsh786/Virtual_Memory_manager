#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <time.h>

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

void handle_page_fault(int page) {
    double fault_start = get_time_ms();
    int frame = find_free_frame();
    
    if (frame == -1) {
        int victim = dequeue();
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
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_CHILDREN, &usage);
    return usage.ru_maxrss;
}

int generate_trace_file(char* binary, char* trace_file) {
    char command[512];
    snprintf(command, sizeof(command), 
             "valgrind --tool=lackey --trace-mem=yes %s 2>&1 | grep ' L ' | head -5000 > %s",
             binary, trace_file);
    return system(command) == 0;
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
    pid_t pid;
    int status;
    double start, end;
    struct rlimit mem_limit;
    
    fflush(stdout);
    start = get_time_ms();
    
    pid = fork();
    if (pid == 0) {
        /* Limit this process to same memory as FIFO */
        mem_limit.rlim_cur = (unsigned long)mem_size_kb * 1024;
        mem_limit.rlim_max = (unsigned long)mem_size_kb * 1024;
        setrlimit(RLIMIT_AS, &mem_limit);
        
        int null_fd = open("/dev/null", O_WRONLY);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
        execl(program, program, (char*)NULL);
        _exit(1);
    } else if (pid > 0) {
        waitpid(pid, &status, 0);
        end = get_time_ms();
        
        info->linux_time = end - start;
        info->memory_kb = get_memory_kb();
        if (info->memory_kb < 4) {
            info->memory_kb = 8;
        }
    }
    fflush(stdout);
}

void run_with_fifo(char* name, long memory_kb, struct program_info* info, char* binary) {
    double start, end;
    char trace_file[512];
    int* trace = NULL;
    int trace_size = 0;
    int num_pages = total_frames * 2;
    int accesses = num_pages * 50;
    
    strcpy(info->name, name);
    info->memory_kb = memory_kb;
    
    snprintf(trace_file, sizeof(trace_file), "%s.trace", binary);
    
    if (generate_trace_file(binary, trace_file)) {
        trace = load_trace(trace_file, &trace_size);
    }
    
    if (trace && trace_size > 0) {
        info->total_accesses = trace_size;
    } else {
        info->total_accesses = accesses;
    }
    
    init_memory();
    
    start = get_time_ms();
    simulate_fifo(trace, trace_size);
    end = get_time_ms();
    
    if (trace) free(trace);
    
    info->fifo_time = end - start;
    info->faults = page_faults;
    info->swaps = swaps;
    info->avg_access_time = info->total_accesses > 0 ? (info->fifo_time / info->total_accesses) : 0.0;
    info->avg_fault_time = info->faults > 0 ? (total_fault_time / info->faults) : 0.0;
    info->avg_swap_out_time = info->swaps > 0 ? (total_swap_out_time / info->swaps) : 0.0;
    info->avg_swap_in_time = swap_ins > 0 ? (total_swap_in_time / swap_ins) : 0.0;
    info->total_io_time = total_swap_out_time + total_swap_in_time;
}

void print_results(struct program_info programs[], int count) {
    int i;
    double avg_fifo = 0;
    double avg_linux = 0;
    int total_faults = 0;
    int total_swaps = 0;
    double total_io = 0.0;
    
    printf("\n");
    printf("  FIFO vs Linux Performance Comparison\n");
    printf("\n");
    printf("Program                  Memory   Faults  Swaps  Avg Fault  Swap Out   Swap In    Total I/O   FIFO Time\n");
    printf("---------------------------------------------------------------------------------------------------------------\n");
    
    for (i = 0; i < count; i++) {
        printf("%-23s %6ld KB  %6d  %5d  %8.4f ms %8.4f ms %8.4f ms %9.2f ms %9.2f ms\n",
               programs[i].name,
               programs[i].memory_kb,
               programs[i].faults,
               programs[i].swaps,
               programs[i].avg_fault_time,
               programs[i].avg_swap_out_time,
               programs[i].avg_swap_in_time,
               programs[i].total_io_time,
               programs[i].fifo_time);
        
        avg_fifo += programs[i].fifo_time;
        avg_linux += programs[i].linux_time;
        total_faults += programs[i].faults;
        total_swaps += programs[i].swaps;
        total_io += programs[i].total_io_time;
    }
    
    printf("---------------------------------------------------------------------------------------------------------------\n");
    printf("\nSummary:\n");
    printf("  Average FIFO Time:   %.2f ms\n", avg_fifo / count);
    printf("  Average Linux Time:  %.2f ms\n", avg_linux / count);
    printf("  Total Page Faults:   %d\n", total_faults);
    printf("  Total Swaps to Disk: %d\n", total_swaps);
    printf("  Total I/O Time:      %.2f ms\n", total_io);
    printf("  Avg Fault Latency:   %.4f ms\n", total_faults > 0 ? (total_io / total_faults) : 0.0);
    printf("  FIFO Overhead:       %.2f%%\n", 
           avg_linux > 0 ? ((avg_fifo - avg_linux) / avg_linux * 100.0) : 0.0);
    printf("\nConfig: %d KB memory, %d KB pages, %d frames, FIFO replacement\n", 
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
    double avg_fifo = 0, avg_linux = 0;
    int total_faults = 0, total_swaps = 0;
    double total_io = 0.0;
    
    if (!f) return;
    
    for (i = 0; i < count; i++) {
        if (programs[i].fifo_time > max_time) max_time = programs[i].fifo_time;
        if (programs[i].linux_time > max_time) max_time = programs[i].linux_time;
        if (programs[i].faults > max_faults) max_faults = programs[i].faults;
        avg_fifo += programs[i].fifo_time;
        avg_linux += programs[i].linux_time;
        total_faults += programs[i].faults;
        total_swaps += programs[i].swaps;
        total_io += programs[i].total_io_time;
    }
    avg_fifo /= count;
    avg_linux /= count;
    
    fprintf(f, "<!DOCTYPE html>\n<html>\n<head>\n");
    fprintf(f, "<meta charset='UTF-8'>\n");
    fprintf(f, "<title>Virtual Memory Manager - Performance Analysis</title>\n");
    fprintf(f, "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>\n");
    fprintf(f, "<style>\n");
    fprintf(f, "* { margin: 0; padding: 0; box-sizing: border-box; }\n");
    fprintf(f, "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); padding: 20px; }\n");
    fprintf(f, ".container { max-width: 1400px; margin: 0 auto; }\n");
    fprintf(f, ".header { background: white; padding: 30px; border-radius: 15px; box-shadow: 0 10px 30px rgba(0,0,0,0.3); margin-bottom: 20px; text-align: center; }\n");
    fprintf(f, "h1 { color: #333; font-size: 2.5em; margin-bottom: 10px; }\n");
    fprintf(f, ".subtitle { color: #666; font-size: 1.1em; }\n");
    fprintf(f, ".stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; margin-bottom: 20px; }\n");
    fprintf(f, ".stat-card { background: white; padding: 25px; border-radius: 15px; box-shadow: 0 5px 15px rgba(0,0,0,0.2); text-align: center; transition: transform 0.3s; }\n");
    fprintf(f, ".stat-card:hover { transform: translateY(-5px); box-shadow: 0 10px 25px rgba(0,0,0,0.3); }\n");
    fprintf(f, ".stat-value { font-size: 2.5em; font-weight: bold; color: #667eea; margin: 10px 0; }\n");
    fprintf(f, ".stat-label { color: #666; font-size: 0.95em; text-transform: uppercase; letter-spacing: 1px; }\n");
    fprintf(f, ".chart-section { background: white; padding: 30px; border-radius: 15px; box-shadow: 0 5px 15px rgba(0,0,0,0.2); margin-bottom: 20px; }\n");
    fprintf(f, ".chart-title { font-size: 1.5em; color: #333; margin-bottom: 20px; border-bottom: 3px solid #667eea; padding-bottom: 10px; }\n");
    fprintf(f, ".chart-container { position: relative; height: 400px; }\n");
    fprintf(f, "table { width: 100%%; border-collapse: collapse; background: white; border-radius: 15px; overflow: hidden; box-shadow: 0 5px 15px rgba(0,0,0,0.2); }\n");
    fprintf(f, "th { background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); color: white; padding: 15px; text-align: left; font-weight: 600; }\n");
    fprintf(f, "td { padding: 12px 15px; border-bottom: 1px solid #eee; }\n");
    fprintf(f, "tr:hover { background: #f5f5f5; }\n");
    fprintf(f, ".note { background: #fff3cd; padding: 20px; border-left: 5px solid #ffc107; margin: 20px 0; border-radius: 10px; }\n");
    fprintf(f, ".note strong { color: #856404; }\n");
    fprintf(f, ".comparison { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; margin-bottom: 20px; }\n");
    fprintf(f, ".footer { text-align: center; color: white; padding: 20px; margin-top: 30px; }\n");
    fprintf(f, ".memory-map { display: grid; grid-template-columns: repeat(auto-fill, minmax(50px, 1fr)); gap: 5px; padding: 20px; }\n");
    fprintf(f, ".frame-box { padding: 10px; border-radius: 5px; text-align: center; font-size: 0.85em; border: 2px solid; }\n");
    fprintf(f, ".frame-occupied { background: #4caf50; color: white; border-color: #2e7d32; }\n");
    fprintf(f, ".frame-free { background: #e0e0e0; color: #666; border-color: #bdbdbd; }\n");
    fprintf(f, ".disk-section { background: #fff8e1; padding: 20px; border-radius: 10px; margin-top: 10px; }\n");
    fprintf(f, ".disk-item { display: inline-block; padding: 5px 10px; margin: 3px; background: #ff9800; color: white; border-radius: 5px; font-size: 0.85em; }\n");
    fprintf(f, "</style>\n");
    fprintf(f, "</head>\n<body>\n");
    
    fprintf(f, "<div class='container'>\n");
    fprintf(f, "<div class='header'>\n");
    fprintf(f, "<h1>Virtual Memory Manager Performance Analysis</h1>\n");
    fprintf(f, "<p class='subtitle'>FIFO Page Replacement vs Linux Native Memory Management</p>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='note'>\n");
    fprintf(f, "<strong>Page Size:</strong> %d KB | <strong>Total Frames:</strong> %d | ", page_size_kb, total_frames);
    fprintf(f, "<strong>Algorithm:</strong> FIFO Replacement\n");
    fprintf(f, "</div>\n");
    
    /* Statistics Cards */
    fprintf(f, "<div class='stats-grid'>\n");
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-label'>Avg FIFO Time</div>\n");
    fprintf(f, "<div class='stat-value'>%.2f ms</div>\n", avg_fifo);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-label'>Avg Linux Time</div>\n");
    fprintf(f, "<div class='stat-value'>%.2f ms</div>\n", avg_linux);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-label'>FIFO Overhead</div>\n");
    fprintf(f, "<div class='stat-value'>%.1f%%</div>\n", 
            avg_linux > 0 ? ((avg_fifo - avg_linux) / avg_linux * 100.0) : 0.0);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-label'>Total Page Faults</div>\n");
    fprintf(f, "<div class='stat-value'>%d</div>\n", total_faults);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-label'>Total Swaps</div>\n");
    fprintf(f, "<div class='stat-value'>%d</div>\n", total_swaps);
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='stat-card'>\n");
    fprintf(f, "<div class='stat-label'>Total I/O Time</div>\n");
    fprintf(f, "<div class='stat-value'>%.2f ms</div>\n", total_io);
    fprintf(f, "</div>\n");
    fprintf(f, "</div>\n");
    
    /* Charts Section */
    fprintf(f, "<div class='comparison'>\n");
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>⏱Execution Time Comparison</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='timeChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Page Faults Distribution</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='faultsChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Average Access Time per Program</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='accessChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>I/O Time Breakdown</div>\n");
    fprintf(f, "<div class='chart-container'><canvas id='ioChart'></canvas></div>\n");
    fprintf(f, "</div>\n");
    
    /* Memory Map Visualization */
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Physical Memory Map</div>\n");
    fprintf(f, "<p style='margin-bottom: 15px; color: #666;'>Visual representation of frame allocation</p>\n");
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
            fprintf(f, "<span class='disk-item'>Page %d</span>", i);
            disk_count++;
            if (disk_count > 20) {
                fprintf(f, "<span class='disk-item'>... and more</span>");
                break;
            }
        }
    }
    if (disk_count == 0) {
        fprintf(f, "<span style='color: #666;'>None</span>");
    }
    fprintf(f, "</div>\n");
    fprintf(f, "</div>\n");
    
    /* Detailed Table */
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Detailed Performance Metrics</div>\n");
    fprintf(f, "<table>\n");
    fprintf(f, "<tr><th>Program</th><th>Memory</th><th>Faults</th><th>Swaps</th><th>Avg Fault</th>");
    fprintf(f, "<th>Swap Out</th><th>Swap In</th><th>Total I/O</th><th>FIFO Time</th></tr>\n");
    
    for (i = 0; i < count; i++) {
        fprintf(f, "<tr><td>%s</td><td>%ld KB</td><td>%d</td><td>%d</td>",
                programs[i].name, programs[i].memory_kb,
                programs[i].faults, programs[i].swaps);
        fprintf(f, "<td>%.4f ms</td><td>%.4f ms</td><td>%.4f ms</td><td style='font-weight:bold;'>%.2f ms</td><td>%.2f ms</td></tr>\n",
                programs[i].avg_fault_time, programs[i].avg_swap_out_time, programs[i].avg_swap_in_time,
                programs[i].total_io_time, programs[i].fifo_time);
    }
    
    fprintf(f, "</table>\n");
    fprintf(f, "</div>\n");
    
    fprintf(f, "</div>\n");
    
    /* JavaScript for Charts */
    fprintf(f, "<script>\n");
    
    /* Program names for labels */
    fprintf(f, "const labels = [");
    for (i = 0; i < count; i++) {
        char short_name[50];
        strncpy(short_name, programs[i].name, 20);
        short_name[20] = '\0';
        fprintf(f, "'%s'%s", short_name, i < count - 1 ? ", " : "");
    }
    fprintf(f, "];\n");
    
    /* Execution Time Chart */
    fprintf(f, "const timeCtx = document.getElementById('timeChart').getContext('2d');\n");
    fprintf(f, "new Chart(timeCtx, {\n");
    fprintf(f, "  type: 'bar',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'FIFO Time (ms)',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].fifo_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(255, 99, 132, 0.7)',\n");
    fprintf(f, "      borderColor: 'rgba(255, 99, 132, 1)',\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }, {\n");
    fprintf(f, "      label: 'Linux Time (ms)',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.2f%s", programs[i].linux_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(75, 192, 192, 0.7)',\n");
    fprintf(f, "      borderColor: 'rgba(75, 192, 192, 1)',\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { responsive: true, maintainAspectRatio: false,\n");
    fprintf(f, "    scales: { y: { beginAtZero: true, title: { display: true, text: 'Time (ms)' }}},\n");
    fprintf(f, "    plugins: { legend: { display: true, position: 'top' }}\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    /* Page Faults Chart */
    fprintf(f, "const faultsCtx = document.getElementById('faultsChart').getContext('2d');\n");
    fprintf(f, "new Chart(faultsCtx, {\n");
    fprintf(f, "  type: 'line',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'Page Faults',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%d%s", programs[i].faults, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(153, 102, 255, 0.2)',\n");
    fprintf(f, "      borderColor: 'rgba(153, 102, 255, 1)',\n");
    fprintf(f, "      borderWidth: 3,\n");
    fprintf(f, "      fill: true,\n");
    fprintf(f, "      tension: 0.4\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { responsive: true, maintainAspectRatio: false,\n");
    fprintf(f, "    scales: { y: { beginAtZero: true, title: { display: true, text: 'Faults' }}},\n");
    fprintf(f, "    plugins: { legend: { display: true, position: 'top' }}\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    /* Access Time Chart */
    fprintf(f, "const accessCtx = document.getElementById('accessChart').getContext('2d');\n");
    fprintf(f, "new Chart(accessCtx, {\n");
    fprintf(f, "  type: 'bar',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'Avg Access Time (ms)',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.4f%s", programs[i].avg_access_time, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(255, 159, 64, 0.7)',\n");
    fprintf(f, "      borderColor: 'rgba(255, 159, 64, 1)',\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }]\n");
    fprintf(f, "  },\n");
    fprintf(f, "  options: { responsive: true, maintainAspectRatio: false,\n");
    fprintf(f, "    scales: { y: { beginAtZero: true, title: { display: true, text: 'Time (ms)' }}},\n");
    fprintf(f, "    plugins: { legend: { display: true, position: 'top' }}\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    /* I/O Time Chart */
    fprintf(f, "const ioCtx = document.getElementById('ioChart').getContext('2d');\n");
    fprintf(f, "new Chart(ioCtx, {\n");
    fprintf(f, "  type: 'bar',\n");
    fprintf(f, "  data: {\n");
    fprintf(f, "    labels: labels,\n");
    fprintf(f, "    datasets: [{\n");
    fprintf(f, "      label: 'Swap Out Time (ms)',\n");
    fprintf(f, "      data: [");
    for (i = 0; i < count; i++) {
        fprintf(f, "%.4f%s", programs[i].avg_swap_out_time * programs[i].swaps, i < count - 1 ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "      backgroundColor: 'rgba(255, 99, 132, 0.7)',\n");
    fprintf(f, "      borderColor: 'rgba(255, 99, 132, 1)',\n");
    fprintf(f, "      borderWidth: 2\n");
    fprintf(f, "    }, {\n");
    fprintf(f, "      label: 'Swap In Time (ms)',\n");
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
    fprintf(f, "  options: { responsive: true, maintainAspectRatio: false,\n");
    fprintf(f, "    scales: { y: { beginAtZero: true, title: { display: true, text: 'Time (ms)' }, stacked: false }},\n");
    fprintf(f, "    plugins: { legend: { display: true, position: 'top' }}\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n");
    
    fprintf(f, "</script>\n");
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
    printf("\nHTML visualization saved to: visualization.html\n");
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
    
    printf("FIFO Virtual Memory Manager\n");
    printf("============================\n");
    printf("Config: %d KB memory, %d KB pages, %d frames\n\n", 
           mem_size_kb, page_size_kb, total_frames);
    
    printf("Compiling programs...\n");
    for (i = 0; i < 10; i++) {
        snprintf(binary_paths[i], 256, "programs/prog%d.out", i);
        if (!compile_program(source_paths[i], binary_paths[i])) {
            printf("  Warning: Failed to compile %s\n", source_paths[i]);
        }
    }
    
    printf("\nRunning comparisons...\n");
    for (i = 0; i < 10; i++) {
        char *name = strrchr(source_paths[i], '/');
        name = name ? name + 1 : source_paths[i];
        
        printf("  %s\n", name);
        run_on_linux(binary_paths[i], &programs[i]);
        run_with_fifo(name, programs[i].memory_kb, &programs[i], binary_paths[i]);
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
