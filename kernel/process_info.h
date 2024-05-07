#define STATE_SIZE 16 // divisible by 4 to avoid pragma pushes
#define NAME_SIZE 16

struct process_info {
  char state[STATE_SIZE];
  int parent_pid;		// in the lowest namespace
  int mem_size;
  int files_count;
  char proc_name[NAME_SIZE];
  uint proc_ticks;
  uint run_time;
  uint context_switches;
};

//  probably should add int my_pid -- pid in the lowest namespace
// to awoid calling both ps_list and ps_list_global in ps.c ps list
