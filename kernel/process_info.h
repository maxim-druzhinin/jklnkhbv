#define NAME_SIZE 16
#define STATE_SIZE 16


struct process_info {
    char state[STATE_SIZE];
    int parent_pid;
    int mem_size;
    int files_count;
    char proc_name[NAME_SIZE];
    //time
    int proc_ticks;
    int run_time;
    int context_switches;
    int user_ticks;
    int kernel_ticks;
    int waiting_ticks;
    //memeory
    int bytes_read;
    int bytes_write;
    int pages_count;
};