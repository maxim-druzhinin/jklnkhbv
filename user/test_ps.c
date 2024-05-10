#include "kernel/types.h"
#include "user/user.h"
#include "kernel/process_info.h"

// !!
// This tests were written by Ulyana, I only fixed print_procs
// and check_pid_exists here, so they're not mine, 
// I used them to manually check the behaviour in test cases


// prints processes in the namespace of the caller (ps_list & ps_info)
// ps.c ps_info is fixed in the same way (using both ps_list and ps_list_global)

void
print_procs(char* indent) {
  int limit = 0;
  int res = ps_list_global(limit, 0);
  if (res < 0)
    return;
  limit = res;
  
  // list of global pids in current namespace, 
  int* pids = malloc(limit * sizeof(int));
  res = ps_list_global(limit, pids);
  int* ns_pids = malloc(limit * sizeof(int));
  int res2 = ps_list(limit, ns_pids);
  
  if (res && res2) {
    struct process_info pi;
    for (int i = 0; i < res; ++i) {
      ps_info(pids[i], &pi);
      printf("%s#%d pid %d ppid %d\n", indent, i, ns_pids[i], pi.parent_pid);
    }
  }
  free(pids);
  free(ns_pids);
}


// check is pid exists in the caller namespace 
// uses ps_list
int
check_pid_exist(int pid) {
  int limit = 0;
  int res = ps_list(limit, 0);
  if (res < 0)
    return res;
  limit = res;
  int* pids = malloc(limit * sizeof(int));
  res = ps_list(limit, pids);
  if (res < 0)
    return res;
  if (res > 0) {
    for (int i = 0; i < limit; ++i) {
      // printf("--> %d\n", pids[i]);
      if (pids[i] == pid)
        return 1;
    }
  }
  return 0;
}


void
test_fork() {
  int pid1 = fork();
  if (pid1 == 0) {
    printf("Child\n");
    exit(0);
    printf("Chil11111\n");
  } else {
    sleep(3);
    printf("Parent\n");
    int st;
    wait(&st);
    printf("Child exited with status: %d\n", st);
  }
}


void
test_clone_fork() {
  printf("This proc %d\n", getpid());


  int pid1 = clone();
  if (pid1 == 0) {
    printf("\tChild #1 %d\n", getpid());


    int pid2 = fork();
    if (pid2 == 0) {
      printf("\t\tChild #2 %d\n", getpid());
      sleep(6);
      exit(0);
    }


    sleep(3);
    printf("\tParent child #2 %d\n", getpid());
    print_procs("\t");
    int st;
    wait(&st);
    printf("\tChild #2 exited with status %d\n", st);
    exit(0);


  } else {
    sleep(3);
    printf("Parent of child #1 %d\n", getpid());
    int st;
    wait(&st);
    printf("Child exited with status: %d\n", st);
  }
}



void
test_task_1_2(char *s) {
  printf("\nRun test for tasks 1 and 2\n");
  for(int i = 0; i < 1; i++) {
    int pid1 = clone();


    if (pid1 < 0) {
      printf("%s: clone failed\n", s);
      exit(1);
    }


    // child code
    if (pid1 == 0) {
      int cp = getpid();
      printf("\tChild pid from itself: %d\n", cp);
      int pp = getppid();
      printf("\tParent pid from child: %d\n", pp);
      sleep(10);
      exit(0);
    }


    sleep(3);
    printf("Child pid from parent: %d\n", pid1);
    int res = check_pid_exist(pid1);
    if (res < 0) {
      printf("%s: check pid failed\n", s);
      exit(1);
    }
    if (res == 0) {
      printf("----------\n");
      printf("looking for pid %d\n", pid1);
      print_procs("\t");
      printf("----------\n");
      printf("%s: child pid does not exist in parent\n");
      exit(1);
    }
    int xst;
    int ch_pid = wait(&xst);
    if (ch_pid != pid1) {
       printf("%s: wait does not return child pid (%d): %d\n", s, pid1, ch_pid);
       exit(1);
    }
    if (xst != 0) {
       printf("%s: status should be 0, but it is %d\n", s, xst);
       exit(1);
    }


    printf("Now child is stopped\n");


    res = check_pid_exist(pid1);
    if (res < 0) {
      printf("%s: check pid failed\n", s);
      exit(1);
    }
    if (res == 1) {
      printf("%s: child pid still exists in parent\n");
      exit(1);
    }
  }
}



void
test_task_3_4(char *s) {
  printf("\nRun test for tasks 3 and 4\n");
  for(int i = 0; i < 1; i++) {
    int pid1 = clone();


    if (pid1 < 0) {
      printf("%s: clone failed\n", s);
      exit(1);
    }


    // child #1 code
    if (pid1 == 0) {
      int pid2 = fork();
      if (pid2 < 0) {
        printf("%s: fork failed", s);
        exit(1);
      }


      // child #2 code
      if (pid2 == 0) {
        int cp = getpid();
        printf("\t\tChild #2 pid from itself: %d\n", cp);
        int res = check_pid_exist(cp);
        if (res < 0) {
          printf("%s: check pid failed\n");
          exit(1);
        }
        if (res == 0) {
          printf("%s: Child #2 does not exist\n");
          exit(1);
        }
        sleep(10);
        exit(0);
      }


      sleep(10);
      printf("\tChild #2 pid from child #1 %d\n", pid2);
      printf("\tProc list from child #1:\n");
      print_procs("\t");


      int st;
      wait(&st);
      exit(0);
    }


    int xst;
    int res = wait(&xst);
    if (res < 0)  {
      printf("%s: wait failed\n");
      exit(1);
    }
    if (xst != 0) {
       printf("%s: status should be 0, but it is %d\n", s, xst);
       exit(1);
    }
  }
}




void
test_task_5(char *s) {
  printf("\nRun test for tasks 5\n");
  printf("Base proc pid %d\n", getpid());
  printf("Proc list from base proc:\n");
  print_procs("");


  for(int i = 0; i < 1; i++) {
    int pid1 = clone();


    if (pid1 < 0) {
      printf("%s: clone failed\n", s);
      exit(1);
    }


    // child #1 code
    if (pid1 == 0) {
      int pid2 = fork();
      if (pid2 < 0) {
        printf("%s: fork failed", s);
        exit(1);
      }


      // child #2 code
      if (pid2 == 0) {
        printf("\t\tChild #2 pid: %d\n", getpid());


        int pid3 = fork();
        if (pid3 < 0) {
          printf("%s: fork failed", s);
          exit(1);
        }


        // third child code
        if (pid3 == 0) {
          printf("\t\t\tChild #3 pid: %d\n", getpid());
          printf("\t\t\tChild #3 parent pid: %d\n", getppid());
          sleep(20);
          printf("\t\t\tChild #3 parent pid after parent exit (should be 1): %d\n", getppid());
          exit(0);
        }


        sleep(20);
        exit(0);
      }


      sleep(5);
      print_procs("\t");
      sleep(20);


      int st;
      int res = wait(&st);
      if (res < 0) {
        printf("%s: wait failed\n");
        exit(1);
      }
      printf("\tProc list after first wait: \n");
      print_procs("\t");


      res = wait(&st);
      if (res < 0) {
        printf("%s: wait failed\n");
        exit(1);
      }
      printf("Proc list after second wait: \n");
      print_procs("\t");


      exit(0);
    }


    int xst;
    int res = wait(&xst);
    if (res < 0)  {
      printf("%s: wait failed\n");
      exit(1);
    }
    if (xst != 0) {
       printf("%s: status should be 0, but it is %d\n", s, xst);
       exit(1);
    }
  }
}



void
test_task_6(char *s) {
  printf("\nRun test for tasks 6\n");
  printf("Base proc pid %d\n", getpid());
  for(int i = 0; i < 1; i++) {


    int pid1 = clone();
    if (pid1 < 0) {
      printf("%s: clone failed\n", s);
      exit(1);
    }


    // child #1 code
    if (pid1 == 0) {
      int pid2 = fork();
      if (pid2 < 0) {
        printf("%s: fork failed\n", s);
        exit(1);
      }


      // child #2 code
      if (pid2 == 0) {
        printf("\t\tChild #2 pid %d\n", getpid());
        printf("\t\tProc list from child #2:\n");
        print_procs("\t\t");
        sleep(10);
        printf("\t\tProc list from child #2:\n");
        print_procs("\t\t");
        sleep(10);
        exit(0);
      }


      sleep(5);
      printf("\tExiting from child #1\n");
      exit(0);
    }


    printf("\tChild #1 pid %d\n", pid1);
    sleep(1);
    printf("Proc list from base proc:\n");
    print_procs("");


    int st;
    wait(&st);


    sleep(3);
    printf("Proc list from base proc:\n");
    print_procs("");
    sleep(30);
    printf("Proc list from base proc:\n");
    print_procs("");
  }
}




void
test_task_7(char *s) {
  printf("\nRun test for tasks 7\n");
  printf("Base proc pid %d\n", getpid());
  for(int i = 0; i < 1; i++) {


    int pid1 = clone();
    if (pid1 < 0) {
      printf("%s: clone failed\n", s);
      exit(1);
    }


    // child #1 code
    if (pid1 == 0) {
      int pid2 = clone();
      if (pid2 < 0) {
        printf("%s: clone failed\n", s);
        exit(1);
      }


      // child #2 code
      if (pid2 == 0) {
        int pid3 = clone();
        if (pid3 < 0) {
          printf("%s: clone failed\n", s);
          exit(1);
        }


        // child #3 code
        if (pid3 == 0) {
          printf("\t\t\tChild #3 pid %d\n", getpid());
          sleep(10);
          exit(0);
        }


        sleep(1);
        printf("\t\tChild #2 pid %d\n", getpid());
        wait(0);


        exit(0);
      }


      sleep(2);
      printf("\tChild #1 pid %d\n", getpid());
      wait(0);
      exit(0);
    }


    wait(0);
  }
}




void
test_task_panic_ns(char *s) {
  printf("\nRun test for tasks panic ns\n");
  printf("Base proc pid %d\n", getpid());
  for(int i = 0; i < 1; i++) {


    int pid1 = clone();
    if (pid1 < 0) {
      printf("%s: clone failed\n", s);
      exit(1);
    }


    // child #1 code
    if (pid1 == 0) {
      int pid2 = clone();
      if (pid2 < 0) {
        printf("%s: clone failed\n", s);
        exit(1);
      }


      // child #2 code
      if (pid2 == 0) {
        int pid3 = clone();
        if (pid3 < 0) {
          printf("%s: clone failed\n", s);
          exit(1);
        }


        // child #3 code
        if (pid3 == 0) {
          int pid4 = clone();
          if (pid4 < 0) {
            printf("%s: clone failed\n", s);
            exit(1);
          }


          // child #4 code
          if (pid4 == 0) {
            int pid5 = clone();
            if (pid5 < 0) {
              printf("%s: clone failed\n", s);
              exit(1);
            }


            // child #5 code
            if (pid5 == 0) {
              int pid6 = clone();
              if (pid6 < 0) {
                printf("%s: clone failed\n", s);
                exit(1);
              }


              // child #6 code
              if (pid6 == 0) {
                int pid7 = clone();
                if (pid7 < 0) {
                  printf("%s: clone failed\n", s);
                  exit(1);
                }


                // child #7 code
                if (pid7 == 0) {
                
                /*
                  int pid8 = clone();
                  if (pid8 < 0) {
                    printf("%s: clone failed (attempted to create 8th level namespace)\n", s);
                    exit(1);
                  }


                  if (pid8 == 0) {
                    printf("Error: too many namespaces\n");
                    exit(-1);
                  }
                  */


                  // sleep(10);
                  printf("\t\t\t\t\t\t\tChild #7 pid %d\n", getpid());
                  // wait(0);
                  exit(0);
                }


                sleep(10);
                printf("\t\t\t\t\t\tChild #6 pid %d\n", getpid());
                wait(0);
                exit(0);
              }


              sleep(12);
              printf("\t\t\t\t\tChild #5 pid %d\n", getpid());
              wait(0);
              exit(0);
            }


            sleep(14);
            printf("\t\t\t\tChild #4 pid %d\n", getpid());
            wait(0);
            exit(0);
          }


          sleep(16);
          printf("\t\t\tChild #3 pid %d\n", getpid());
          wait(0);
          exit(0);
        }


        sleep(18);
        printf("\t\tChild #2 pid %d\n", getpid());
        wait(0);
        exit(0);
      }


      sleep(20);
      printf("\tChild #1 pid %d\n", getpid());
      wait(0);
      exit(0);
    }


    wait(0);
  }
}


int
main(int argc, char *argv[])
{
    printf("HELLO!");
  test_fork();
  printf("HELLO!");
  test_clone_fork();
  printf("HELLO!");
  test_task_1_2("Test clone, task 1 and 2");
  test_task_3_4("Test clone, task 3 and 4");
  test_task_5("Test clone task 5");
  test_task_6("Test clone task 6");
  test_task_7("Test clone task 7");
  test_task_panic_ns("Test deep limit");
  printf("All done.\n");
  return 0;
}
