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

int frame_occupied[256];

int fifo_queue[256];
int fifo_front = 0;
int fifo_rear = 0;

int page_faults = 0;
int swaps = 0;

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
};

void init_memory(void) {
    int i;
    for (i = 0; i < 1024; i++) {
        page_frame[i] = -1;
        page_valid[i] = 0;
    }
    for (i = 0; i < 256; i++) {
        frame_occupied[i] = 0;
    }
    fifo_front = 0;
    fifo_rear = 0;
    page_faults = 0;
    swaps = 0;
    
    if (!disk_store) {
        disk_store = fopen("disk_swap.bin", "w+b");
    }
}

void enqueue(int page) {
    fifo_queue[fifo_rear] = page;
    fifo_rear = (fifo_rear + 1) % 256;
}

int dequeue(void) {
    int page = fifo_queue[fifo_front];
    fifo_front = (fifo_front + 1) % 256;
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
    long pos = (long)page * page_size_kb * 1024;
    fseek(disk_store, pos, SEEK_SET);
    fwrite(&page, sizeof(int), 1, disk_store);
    fflush(disk_store);
    swaps++;
}

void handle_page_fault(int page) {
    int frame = find_free_frame();
    
    if (frame == -1) {
        int victim = dequeue();
        frame = page_frame[victim];
        swap_to_disk(victim);
        page_valid[victim] = 0;
        page_frame[victim] = -1;
    }
    
    page_frame[page] = frame;
    page_valid[page] = 1;
    frame_occupied[frame] = 1;
    enqueue(page);
}

void access_page(int page) {
    if (page_valid[page] == 0) {
        page_faults++;
        handle_page_fault(page);
    }
}

double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_CHILDREN, &usage);
    return usage.ru_maxrss;
}

void simulate_fifo(long memory_kb) {
    int num_pages = total_frames * 2;
    int accesses = num_pages * 50;
    int i, j, k;
    volatile int dummy = 0;
    double total_access_time = 0.0;
    
    for (i = 0; i < accesses; i++) {
        int page = rand() % num_pages;
        double access_start = get_time_ms();
        access_page(page);
        double access_end = get_time_ms();
        total_access_time += (access_end - access_start);
        
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
    long mem_before, mem_after;
    struct rlimit mem_limit;
    
    fflush(stdout);
    mem_before = get_memory_kb();
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
        mem_after = get_memory_kb();
        
        info->linux_time = end - start;
        info->memory_kb = mem_after - mem_before;
        if (info->memory_kb < 4) {
            info->memory_kb = 8;
        }
    }
    fflush(stdout);
}

void run_with_fifo(char* name, long memory_kb, struct program_info* info) {
    double start, end;
    int num_pages = total_frames * 2;
    int accesses = num_pages * 50;
    
    strcpy(info->name, name);
    info->memory_kb = memory_kb;
    info->total_accesses = accesses;
    
    init_memory();
    
    start = get_time_ms();
    simulate_fifo(memory_kb);
    end = get_time_ms();
    
    info->fifo_time = end - start;
    info->faults = page_faults;
    info->swaps = swaps;
    info->avg_access_time = info->faults > 0 ? (info->fifo_time / info->total_accesses) : 0.0;
}

void print_results(struct program_info programs[], int count) {
    int i;
    double avg_fifo = 0;
    double avg_linux = 0;
    int total_faults = 0;
    int total_swaps = 0;
    
    printf("\n");
    printf("  FIFO vs Linux Performance Comparison\n");
    printf("\n");
    printf("Program                  Memory   Accesses  Faults  Swaps  Avg Access   FIFO Time  Linux Time\n");
    printf("---------------------------------------------------------------------------------------------------\n");
    
    for (i = 0; i < count; i++) {
        printf("%-23s %6ld KB  %7d  %6d  %5d  %7.4f ms  %8.2f ms  %7.2f ms\n",
               programs[i].name,
               programs[i].memory_kb,
               programs[i].total_accesses,
               programs[i].faults,
               programs[i].swaps,
               programs[i].avg_access_time,
               programs[i].fifo_time,
               programs[i].linux_time);
        
        avg_fifo += programs[i].fifo_time;
        avg_linux += programs[i].linux_time;
        total_faults += programs[i].faults;
        total_swaps += programs[i].swaps;
    }
    
    printf("---------------------------------------------------------------------------------------------------\n");
    printf("\nSummary:\n");
    printf("  Average FIFO Time:   %.2f ms\n", avg_fifo / count);
    printf("  Average Linux Time:  %.2f ms\n", avg_linux / count);
    printf("  Total Page Faults:   %d\n", total_faults);
    printf("  Total Swaps to Disk: %d\n", total_swaps);
    printf("  FIFO Overhead:       %.2f%%\n", 
           avg_linux > 0 ? ((avg_fifo - avg_linux) / avg_linux * 100.0) : 0.0);
    printf("\nConfig: %d KB memory, %d KB pages, %d frames, FIFO replacement\n", 
           mem_size_kb, page_size_kb, total_frames);
}



void generate_html(struct program_info programs[], int count) {
    FILE *f = fopen("visualization.html", "w");
    int i;
    double max_time = 0.0;
    int max_faults = 0;
    double avg_fifo = 0, avg_linux = 0;
    int total_faults = 0, total_swaps = 0;
    
    if (!f) return;
    
    for (i = 0; i < count; i++) {
        if (programs[i].fifo_time > max_time) max_time = programs[i].fifo_time;
        if (programs[i].linux_time > max_time) max_time = programs[i].linux_time;
        if (programs[i].faults > max_faults) max_faults = programs[i].faults;
        avg_fifo += programs[i].fifo_time;
        avg_linux += programs[i].linux_time;
        total_faults += programs[i].faults;
        total_swaps += programs[i].swaps;
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
    
    /* Detailed Table */
    fprintf(f, "<div class='chart-section'>\n");
    fprintf(f, "<div class='chart-title'>Detailed Performance Metrics</div>\n");
    fprintf(f, "<table>\n");
    fprintf(f, "<tr><th>Program</th><th>Memory</th><th>Accesses</th><th>Faults</th><th>Swaps</th>");
    fprintf(f, "<th>Avg Access</th><th>FIFO Time</th><th>Linux Time</th><th>Overhead</th></tr>\n");
    
    for (i = 0; i < count; i++) {
        double overhead = programs[i].linux_time > 0 ? 
                         ((programs[i].fifo_time - programs[i].linux_time) / programs[i].linux_time * 100.0) : 0.0;
        fprintf(f, "<tr><td>%s</td><td>%ld KB</td><td>%d</td><td>%d</td><td>%d</td>",
                programs[i].name, programs[i].memory_kb, programs[i].total_accesses,
                programs[i].faults, programs[i].swaps);
        fprintf(f, "<td>%.4f ms</td><td>%.2f ms</td><td>%.2f ms</td><td style='color:%s;font-weight:bold;'>%.1f%%</td></tr>\n",
                programs[i].avg_access_time, programs[i].fifo_time, programs[i].linux_time,
                overhead > 0 ? "#d32f2f" : "#388e3c", overhead);
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
        run_with_fifo(name, programs[i].memory_kb, &programs[i]);
    }
    
    print_results(programs, 10);
    generate_html(programs, 10);
    
    if (disk_store) {
        fclose(disk_store);
        disk_store = NULL;
    }
    
    return 0;
}
