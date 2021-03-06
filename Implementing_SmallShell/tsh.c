/* 
 * tsh - A tiny shell program with job control
 * 
 * Alan Chen 31271652
 * Chidinma Ejiofor 30773084
 * 
 * ECE322 Assignment 3 - Managing Shell
 * 
 * Work Cited Page:
 * https://www.gnu.org/software/libc/manual/html_node/Process-Signal-Mask.html
 * https://stackoverflow.com/questions/6757188/why-sigprocmask-is-used-to-block-sigchld-from-delivering-in-the-following-code
 * https://stackoverflow.com/questions/33508997/waitpid-wnohang-wuntraced-how-do-i-use-these
 * http://www.cs.cmu.edu/afs/cs/academic/class/15213-f13/www/recitations/rec9_anitazha.pdf
 * https://gist.github.com/seanrivera
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);
void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);
void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) {
    /*
     * The sigprocmask function is used to examine or change the calling process’s signal mask. The how argument determines how the signal mask is changed, and must be one of the following values:
     * SIG_BLOCK
     * Block the signals in set—add them to the existing mask. In other words, the new mask is the union of the existing mask and set.
     * SIG_UNBLOCK
     * Unblock the signals in set—remove them from the existing mask.
     * SIG_SETMASK
     * Use set for the mask; ignore the previous value of the mask.
     * --------------------STEPS TO COMPLETE--------------------
     * 1. need to check if it is a valid argument
     * 2. check if it is a buildin function
        * Yes - execute the build in function (jobs, exit, bg, fg)
        * No
            * 0. We need to declare empty set, add sigChld, block for parent, 
                * and then unblock when children executes to make sure we do not
                * miss any signal from child since it is a race condition
            * 1. Stop all processes while running
            * 2. Fork than run the new process in child
            * 3. if it is bg than add job to background job list, else foreground job list and waitfg
    */
    char *argv[MAXARGS];        // Argument list execve()
    char buf[MAXLINE];          // Holds modified copppmand line
    strcpy(buf, cmdline);
    sigset_t mask;              // declares a signal set type
    int bg = parseline(buf, argv);  //Process ID, true = background, false = foreground
    if (argv[0] == NULL){    //Ignore empty lines
        return;
    }
    if(!builtin_cmd(argv)) { // will execute builtin command if found
        //------Signal Blocking Starts -----------
        sigemptyset(&mask);         //initializes the signal set set to exclude all of the defined signals. It always returns 0.
        sigaddset(&mask,SIGCHLD);   //This function adds the signal signum to the signal set set. All sigaddset does is modify set; it does not block or unblock any signals.
        sigprocmask(SIG_BLOCK, &mask,NULL); //blocked signals for parent
        pid_t pid;
        if((pid = fork()) == 0){    // makes a child, then executes the command (pid inside child (inside the if loop)is 0)
            setpgrp();
            if(execve(argv[0], argv, environ) < 0){
                printf("%s: Command not found.\n", argv[0]); 
                exit(0);
            }
        }
        if (!bg) {  /* Parent waits for foreground "child" job to terminate */
            int worked = addjob(jobs, pid, FG, cmdline);
            if (worked) {
            sigprocmask(SIG_UNBLOCK,&mask,NULL);    // Unblock for parent
            waitfg(pid);
            }else{
                kill(-pid,SIGINT); //Else kill the child
            }
        }
        else{   /* If it is a background command do something ... */
            int worked = addjob(jobs, pid, BG, cmdline);
            if (worked){
                sigprocmask(SIG_UNBLOCK,&mask,NULL);    // Unblock for parent
                printf("[%d] (%d) %s",pid2jid(pid), pid, cmdline);
            }else{
                kill(-pid,SIGINT); //Else kill the child
            }
        }
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    if (!strcmp(argv[0], "quit")){ /* quit command */
	    exit(0);
        return 1;
    } else if (!strcmp(argv[0], "jobs")){   //revise need to do the jobs
        listjobs(jobs);
        return 1;   
    } else if (!strcmp(argv[0], "bg") || !strcmp(argv[0], "fg")){    //foreground and background
        do_bgfg(argv);
        return 1;
    } else {
        return 0;
    }
}

void do_bgfg(char **argv) {
    /* 
    * do_bgfg put process either in the backgroud or foreground
    *   
    * E.G bg %1
    * 
    * 1. the first argument is either fg or bg as confirmed in builtin command
    * 2. need to check if they have an approiate argument
    * 3. check if the second argument starts with % else invalid
    * 4. check if the what follows % is a number and a valid pid  
    * 5. Sent Cont Signal and set new states
    */
    struct job_t *comp_job;
    if(argv[1] == NULL){
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;    
    }
    else if (isdigit(argv[1][0])) {
        pid_t pid = atoi(argv[1]);
        if (!(comp_job = getjobpid(jobs, pid))) {
        printf("(%d): No such process\n", pid);
        return;
        }
    } 
    else if (argv[1][0] == '%') {
        int jid = atoi(&argv[1][1]);
        if (!(comp_job= getjobjid(jobs, jid))) {
            printf("%s: No such job\n", argv[1]);
            return;
        }
    }
    else if (!(isdigit(argv[1][0])) || argv[1][0] != '%') { // if it is not a digit or
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
    }
    kill(-comp_job->pid,SIGCONT); // for both bg and fg you need to send cont signal to run it
    if(!strcmp(argv[0], "bg")){    // set new state
	    comp_job->state = BG;
        printf("[%d] (%d) %s",comp_job->jid,comp_job->pid,comp_job->cmdline);
    } else {    // set new state
        comp_job->state = FG;
	    waitfg(comp_job->pid);  // wait for foreground to finish
    }
    return;
}


/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid){
	while( fgpid(jobs) == pid );    // if the foreground of the fgpid is process pid then wait 
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    /*
     * WUNTRACED = return when child is stuck 
     * WNOHANG = return immediately instead of waiting, if there is no child process ready to be noticed 
     * WIFSTOPPED = check if stopped
     * WIFSIGNALED = check if child has been terminated
     * WIFEXITED = check if child is exit
     * WSTOPSIG = gives us the number of which signal(PID) has been stopped
     * WTERMSIG = gives us the number of which signal(PID) has been terminated
     * WIFCONTINUED = check if the signal has been continued
    */
	int chld_status = -1;
	while(1) {
		pid_t wait_pid = waitpid((pid_t) -1, &chld_status, (WUNTRACED | WNOHANG));  // this waits for any signal
		if(wait_pid < 1){							// If interrupted or no status available, stop handler
			break;
        } else if(WIFSIGNALED(chld_status)) {		// Child terminated by signal
			printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(wait_pid), wait_pid, WTERMSIG(chld_status));
			deletejob(jobs, wait_pid);			
		} 
        else if(WIFCONTINUED(chld_status)) {      // if a child signal has been continued set to foreground and waitfg
            getjobpid(jobs, wait_pid)->state = FG;
            waitfg(wait_pid);
        } 
        else if(WIFSTOPPED(chld_status)) {				// Child stopped
			printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(wait_pid), wait_pid, WSTOPSIG(chld_status));
			getjobpid(jobs, wait_pid)->state = ST;	// Set job state
		} else if(WIFEXITED(chld_status)){			        // Child exited
			deletejob(jobs, wait_pid);		
        }
	}
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) {
    // 0. get foreground id
    // 1. send interupt signal to foreground id
    pid_t fgid=fgpid(jobs);
    kill(-fgid, SIGINT);
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */

void sigtstp_handler(int sig) {
    // 0. get foreground id
    // 1. stops foreground id
    // 2. set new state
	pid_t fgid=fgpid(jobs);
    if(fgid) {
		kill(-fgid,SIGTSTP);
    	(*getjobpid(jobs,fgid)).state = ST;
	}	
  return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



