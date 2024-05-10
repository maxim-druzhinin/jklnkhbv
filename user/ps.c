#include "stddef.h"
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/process_info.h"
#include "user/user.h"


void process_count() {
    int count_ps = ps_list(-1, NULL);
    if (count_ps == -1) {
        printf("Error count\n");
        exit(-1);
    } else {
        printf("%d\n", count_ps);
    }
}

void process_pids() {
    int limit = ps_list(-1, NULL);
	int pids[limit];
	memset(pids, -1, sizeof(pids));

	int count_ps = ps_list(limit, pids);
	if (count_ps == -1) {
	    printf("ps_list: internal error\n");
	    exit(-1);
	} else {
	    printf("Total: %d\n", count_ps);
        printf("\nPids:\n");
	    for (int i = 0; i < limit; ++i) {
	        printf("%d ", pids[i]);
	    }
	    printf("\n");
    }
}

void process_list(){
    int limit = 100;
	int pids[limit];  // global pids
	int pids_ns[limit];  // namespace pids
	for (int i = 0; i < limit; ++i) {
        pids[i] = -1;
	    pids_ns[i] = -1;
	}
	  
	int count_ps = ps_list_global(limit, pids);
	if (count_ps < 0) {
	    printf("ps_list (global): internal error\n"); 
	}
	if (count_ps > limit) {
	    printf("ps_list: too many processes\n");
	    exit(-1);
	}
	  
	int res = ps_list(limit, pids_ns);
	if (res < 0) {
	    printf("ps_list (namespace): internal error\n"); 
	}





    for (int i = 0; i < count_ps; ++i) {
        struct process_info psinfo = {}; // Initialize all fields
        int res = ps_info(pids[i], &psinfo); // Retrieve process info

        if (res == -1) {
            printf("ps_info: cannot get info about global pid = %d\n\n", pids[i]);
        } else {
            // Display all process information
            printf(
                "info about pid = %d in namespace %d:\n"
                "state -> %s\n"
                "PPID -> %d\n"
                "mem -> %d bytes\n"
                "files_count -> %d\n"
                "name -> %s\n"
                "/////TIME/////:\n"
                "proc_ticks -> %d\n"
                "run_time -> %d\n"
                "context_switches -> %d\n"
                "user -> %d\n"
                "kernel -> %d\n"
                "waiting -> %d\n"
                "/////MEMINFO/////:\n"
                "read -> %d\n"
                "write -> %d\n"
                "pages -> %d\n"
                "ps_info return value -> %d\n\n",
                pids[i], pids_ns[i],
                psinfo.state, psinfo.parent_pid, psinfo.mem_size, 
                psinfo.files_count, psinfo.proc_name, 
                psinfo.proc_ticks, psinfo.run_time, psinfo.context_switches,
                psinfo.user_ticks, psinfo.kernel_ticks, psinfo.waiting_ticks,
                psinfo.bytes_read, psinfo.bytes_write, psinfo.pages_count,
                res
            );
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Argument_ERROR: use \"ps count\", \"ps pids\" or \"ps list\"\n");
       exit(0);
    }

    if (!strcmp(argv[1], "count")) {
        process_count();
    } else if (!strcmp(argv[1], "pids")) {
        process_pids();
    } else if (!strcmp(argv[1], "list")) {
        process_list();
    } else {
        printf("Unknown command: ps %s\n", argv[1]);
        exit(1);
    }

  exit(0);
}