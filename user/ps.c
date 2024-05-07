#include "stddef.h"

#include "kernel/types.h"
#include "kernel/process_info.h"
#include "user/user.h"

void
main(int argc, char *argv[]) {

	if (argc != 2) {
	  printf("use \"ps count\", \"ps pids\" or \"ps list\"\n");
	  exit(0);
	}
	
	// ps count uses ps_list to count pids in the current namespace
	if (!strcmp(argv[1], "count")) {
	
	  int proc_cnt = ps_list(-1, NULL);
	  
	  if (proc_cnt == -1) {
	    printf("error\n");
	    exit(-1);
	  } 
	  else {
	    printf("%d\n", proc_cnt);
	  }
	  
	}
	
	// ps pids uses ps_list to get pids in the current namespace
	else if (!strcmp(argv[1], "pids")) {
	
	  int limit = ps_list(-1, NULL);
	  int pids[limit];
	  for (int i = 0; i < limit; ++i) {
	    pids[i] = -1;
	  }
	  
	  int proc_cnt = ps_list(limit, pids);
	  if (proc_cnt == -1) {
	    printf("ps_list: internal error\n");
	    exit(-1);
	  }
	  else {
	  printf("total: %d\n", proc_cnt);
	  for (int i = 0; i < limit; ++i) {
	      printf("%d ", pids[i]);
	  }
	  printf("\n");
	  }
	   
	}
	
	// ps pids uses ps_list to get pids in the current namespace
	// and ps_list_global as ps_info needs global pids
	else if (!strcmp(argv[1], "list")) {
	
	  int my_limit = 100;
	  int my_pids[my_limit];  // global pids
	  int my_ns_pids[my_limit];  // namespace pids
	  for (int i = 0; i < my_limit; ++i) {
	    my_pids[i] = -1;
	    my_ns_pids[i] = -1;
	  }
	  
	  int proc_cnt = ps_list_global(my_limit, my_pids);
	  if (proc_cnt < 0) {
	    printf("ps_list (global): internal error\n"); 
	  }
	  if (proc_cnt > my_limit) {
	    printf("ps_list: too many processes\n");
	    exit(-1);
	  }
	  
	  int res = ps_list(my_limit, my_ns_pids);
	  if (res < 0) {
	    printf("ps_list (namespace): internal error\n"); 
	  }
	  
	  for (int i = 0; i < proc_cnt; ++i) {
	  
	    struct process_info psinfo = {};
	    int res = ps_info(my_pids[i], &psinfo);
	    
	    if (res == -1) {
	    
	      printf("ps_info: cannot get info about pid (global) = %d\n\n", my_pids[i]);
	      
	    }
	    else {
	    
	     printf("info about pid (namespace) %d (global) %d:\n", my_ns_pids[i], my_pids[i]);
	     printf("state = %s\n", psinfo.state);
	     printf("parent_id (namespace) = %d\n", psinfo.parent_pid);
	     printf("mem_size = %d bytes\n", psinfo.mem_size);
	     printf("files_count = %d\n", psinfo.files_count);
	     printf("proc_name = %s\n", psinfo.proc_name);
	     printf("proc_ticks = %d\n", psinfo.proc_ticks);
	     printf("run_time = %d\n", psinfo.run_time);
	     printf("context_switches = %d\n", psinfo.context_switches);
	     printf("ps_info return value = %d\n", res);
	     printf("\n");
	     
	    }
	  } 
	  
	} else {
	
	  printf("unknown command: ps %s\n", argv[1]);
	  exit(1);
	
	}
	
	exit(0);
	
}
