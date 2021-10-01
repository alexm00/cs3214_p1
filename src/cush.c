/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
//static char custom_prompt[] = "/! /u@/h in /W";

static void handle_child_status(pid_t pid, int status);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

static char *
build_prompt(int* com_num){
    (*com_num) += 1;
	return strdup("cush > ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */
	
    /* Add additional fields here if needed. */
	int pid;
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job* jid2job[MAXJOBS];

static int stopped_jobs[MAXJOBS];
static int num_stop_job = 0;

static void add_stopped_job(int jid){
	stopped_jobs[num_stop_job] = jid;
	num_stop_job++;
}

static void start_stopped_job(int jid){
	bool started = false;
	for(int i = 0; i < num_stop_job; i++){
		if(!started && jid == stopped_jobs[i]){
			started = true;
		}
		if(started){
			if(i < num_stop_job-1){
				stopped_jobs[i] = stopped_jobs[i+1];
			}
			else if(i == num_stop_job - 1){
				stopped_jobs[i] = 0;
			}
		}
	}
	if(started){
		num_stop_job--;
	}
}

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
	
	if(pipe->bg_job){
		job->status = BACKGROUND;
	}
	
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

static void
handle_child_status(pid_t pid, int status){
	
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */
	
	if(pid > 0){
		struct job* j = NULL;
		for (struct list_elem * e = list_begin(&job_list); 
		e != list_end(&job_list); 
		e = list_next(e)) {
			j = list_entry(e, struct job, elem);
			if(j->pid == pid){
				break;
			}
		}
	
		if(j == NULL){
			utils_fatal_error("Error: There are currently no known jobs to recieve signal from");
		}
		else if(pid != j->pid){
			utils_fatal_error("Error: No job was found matching the ended process pid");
		}
		else{
			if(WIFEXITED(status) || WIFSIGNALED(status)){
				//check for num_process_alive???
				j->num_processes_alive--;
			}
			else if(WIFSTOPPED(status)){
				j->status = STOPPED;
				int stop_sig = WSTOPSIG(status);
				if(j->status == FOREGROUND){
					termstate_save(&j->saved_tty_state); //save tty state
					print_job(j);
				}
				else{
					if(stop_sig == SIGTTOU || stop_sig == SIGTTIN){
						j->status = NEEDSTERMINAL;
					}
					else{
						print_job(j);
					}
				}
				add_stopped_job(j->jid);
			}
			termstate_give_terminal_back_to_shell();
		}
	}
	else{
		utils_fatal_error("Error in waiting for signal from child process");
	}
}




static void execute(struct ast_pipeline* pipeline){
	
	struct job* cur_job = add_job(pipeline);
				
	signal_block(SIGCHLD);
				
	int pid = fork();
	
	//if child
	if(pid == 0){
		
		signal_unblock(SIGCHLD);
		
		//make pipes
		int size = list_size(&pipeline->commands);
		
		int pipes[size][2];
		for(int i = 0; i < size+1; i++){
			pipe(pipes[i]);
		}
		
		//int READ_END = 0;
		//int WRITE_END = 1;
		
		//input file
		int input_fd = -1;
		if(pipeline->iored_input != NULL){
			input_fd = open(pipeline->iored_input, O_RDONLY);
		}
		//output file
		int output_fd = -1;
		if(pipeline->iored_output != NULL){
			if(pipeline->append_to_output){
				output_fd = open(pipeline->iored_output, O_WRONLY | O_CREAT | O_APPEND, 0750);
			}
			else{
				output_fd = open(pipeline->iored_output, O_WRONLY | O_CREAT, 0750);
			}
		}
		
		int com_num= 0;
		int pid = 0;
		
		signal_block(SIGCHLD);
		
		//parse pipeline
		for (struct list_elem * e = list_begin(&pipeline->commands); 
		e != list_end(&pipeline->commands); 
		e = list_next(e)) {
			struct ast_command* cmd = list_entry(e, struct ast_command, elem);
			pid = fork();
			
			if(pid == 0){
				
				//pipeline pipes
				for(int i = 0; i < size; i++){
					if(i == com_num){
						if(i == 0){
							if(input_fd > 0){
								dup2(pipes[i][0], STDIN_FILENO);
							}
							//if first command, but no input file, leave stdin alone to read from terminal
						}
						else{
							dup2(pipes[i][0], STDIN_FILENO);
						}
					}
					else if(i == (com_num + 1)){
						if(i == size){
							if(output_fd > 0){
								dup2(output_fd, STDOUT_FILENO);
							}
							//if final command, but not output to file, leave stdout alone to print to terminal
						}
						else{
							dup2(pipes[i][1], STDOUT_FILENO);
						}
					}
					
					if(i < size){
						close(pipes[i][0]);
						close(pipes[i][1]);
					}
				}
				
				if(cmd->dup_stderr_to_stdout){
					dup2(STDOUT_FILENO, STDERR_FILENO);
				}
				
				//execute
				execvp(*cmd->argv, cmd->argv);
				
				//if execute failed
				printf("Command execution error\n");
				exit(0);
			}
			cur_job->num_processes_alive++;
			com_num++;
		}
		
		//parent pipes
		//leave following pipes open:
		//pipe[0][1] for writing to first cmd
		//pipe[size][0] for reading from last cmd
		for(int i = 0; i < size; i++){
			if(i == 0){
				if(input_fd > 0){
					close(pipes[i][0]);
				}
				else{
					close(pipes[i][0]);
					close(pipes[i][1]);
				}
			}
			/*else if(i == size){
				if(output_fd == -1){
					close(pipes[i][1]);
				}
				else{
					close(pipes[i][0]);
					close(pipes[i][1]);
				}
			}*/
			else{
				close(pipes[i][0]);
				close(pipes[i][1]);
			}
		}
		
		//read from file to pipe
		if(input_fd > 0){
			char* line;
			size_t max_size = 128;
			int size = 0;
			FILE* in = fdopen(input_fd, "r");
			while((size = getline(&line, &max_size, in)) >= 0){
				line[size] = '\0';
				write(pipes[0][1], line, 128);
			}
			//close input fd's
			fclose(in);
			close(input_fd);
			close(pipes[0][1]);
		}
		
		//catch child processes???
		while(cur_job->num_processes_alive > 0){
			int status, id;
			id = waitpid(-1, &status, WNOHANG);
			if(id){
				cur_job->num_processes_alive--;
			}
		}
		
		signal_unblock(SIGCHLD);
		
		//close remainging output fds
		if(output_fd > 0){
			close(output_fd);
		}
		
		exit(0);
	}
	
	//set job values from child process
	cur_job->num_processes_alive++;
	cur_job->pid = pid;
	
	//save good terminal state
	termstate_save(&cur_job->saved_tty_state);
	
	//set child process pgid
	setpgid(pid, 0);
	
	//if job is foreground
	if(cur_job->status == FOREGROUND){
		termstate_give_terminal_to(&cur_job->saved_tty_state, cur_job->pid);
		wait_for_job(cur_job);
	}
	else if(cur_job->status == BACKGROUND){
		printf("[%d] %d\n", cur_job->jid, cur_job->pid);
	}
	
	termstate_give_terminal_back_to_shell();
	
	signal_unblock(SIGCHLD);
}






static void run_pipeline(struct ast_pipeline* pipe){
	
	//parse pipeline for command arguments, determine validity of built-in commands, and retrieve job number for appropriate builtins;
	//get frst command ni pipeline
	struct ast_command* com = list_entry(list_begin(&pipe->commands), struct ast_command, elem);
	//determnie how many arguments command has
	char** cmd_argv = com->argv;
	int argc = 0;
	while(*(cmd_argv + argc) != NULL){
		argc++;
	}
	
	if(strcmp(*cmd_argv, "exit") == 0){
		exit(0);
	}
	else if(strcmp(*cmd_argv, "kill") == 0){
		if(argc == 2){
			int jid = atoi(*(cmd_argv + 1));
			struct job* j = get_job_from_jid(jid);
			if(j == NULL){
				printf("jid: %d was not found among the current jobs\n", jid);
			}
			else{
				int ret_status = killpg(j->pid, SIGTERM);
				if(ret_status >= 0){
					list_remove(&j->elem);
				}
				else{
					printf("Kill on job: %d was unsuccessful\n", jid);
				}
				
			}
		}
		else{
			printf("Incorrect number of arguments for command 'kill'\n");
		}
	}
	else if(strcmp(*cmd_argv, "stop") == 0){
		if(argc == 2){
			int jid = atoi(*(cmd_argv + 1));
			struct job* j = get_job_from_jid(jid);
			if(j == NULL){
				printf("jid: %d was not found among the current jobs\n", jid);
			}
			else{
				int ret_status = killpg(j->pid, SIGSTOP);
				if(ret_status >= 0){
					j->status = STOPPED; //set status here?
					termstate_save(&j->saved_tty_state);
				}
				else{
					printf("Stop on job: %d was unsuccessful\n", jid);
				}
			}
		}
		else{
			printf("Incorrect number of arguments for command 'stop'\n");
		}
	}
	else if(strcmp(*cmd_argv, "jobs") == 0){
		if(argc == 1){
			if(!list_empty(&job_list)){
				for (struct list_elem * e = list_begin(&job_list); 
				e != list_end(&job_list); 
				e = list_next(e)) {
					struct job* j = list_entry(e, struct job, elem);
					print_job(j);
				}
			}
			else{
				printf("There are currently no jobs\n");
			}
		}
		else{
			printf("Incorrect number of arguments for command 'jobs'\n");
		}
	}
	else if(strcmp(*cmd_argv, "fg") == 0){
		
		struct job* j = NULL;
		int jid = 0;
		
		if(argc == 1){
				//get job from last stopped job
				if(num_stop_job > 0){
					j = get_job_from_jid(stopped_jobs[num_stop_job-1]);
					jid = j->jid;
				}
				else{
					printf("There are currently no stopped jobs\n");
					return;
				}
		}
		else if(argc == 2){
			jid = atoi(*(cmd_argv + 1));
			j = get_job_from_jid(jid);
			if(j->status != STOPPED){
				printf("Job: %d is already running\n", jid);
				return;
			}
		}
		else{
			printf("Incorrect number of arguments for command 'fg'\n");
			return;
		}
		
		if(j == NULL){
			printf("There was no stopped job found\n");
			return;
		}
		else{
			//set tty state
			signal_block(SIGCHLD);
			int ret_status = killpg(j->pid, SIGCONT);
			if(ret_status >= 0){
				start_stopped_job(j->jid);
				termstate_give_terminal_to(&j->saved_tty_state, j->pid);
				j->status = FOREGROUND;
				print_job(j);
				wait_for_job(j);
			}
			else{
				printf("fg on job: %d was unsuccessful\n", jid);
			}
			signal_unblock(SIGCHLD);
			termstate_give_terminal_back_to_shell();
		}
		
	}
	else if(strcmp(*cmd_argv, "bg") == 0){
		
		struct job* j = NULL;
		int jid = 0;
		
		if(argc == 1){
			//get job from last stopped job
				if(num_stop_job > 0){
					j = get_job_from_jid(stopped_jobs[num_stop_job-1]);
					jid = j->jid;
				}
				else{
					printf("There are currently no stopped jobs\n");
					return;
				}
		}
		else if(argc == 2){
			jid = atoi(*(cmd_argv + 1));
			j = get_job_from_jid(jid);
			if(j == NULL){
				printf("There are currently no stopped jobs\n");
				return;
			}
			else if(j->status != STOPPED){
				printf("Job: %d is already running\n", jid);
				return;
			}
		}
		else{
			printf("Incorrect number of arguments for command 'bg'\n");
			return;
		}
			
		if(j == NULL){
			printf("There are currently no stopped jobs\n");
			return;
		}
		else{
			int ret_status = killpg(j->pid, SIGCONT);
			if(ret_status >= 0){
				start_stopped_job(j->jid);
				j->status = BACKGROUND;
				print_job(j);
			}
			else{
				printf("bg on job: %d was unsuccessful\n", jid);
			}
			termstate_give_terminal_back_to_shell();
		}
	}
	else{
		execute(pipe);
	}
}

int main(int ac, char *av[]){
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

	int com_num = 0;
    /* Read/eval loop. */
    for (;;) {

        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt(&com_num) : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        struct ast_command_line * cline = ast_parse_command_line(cmdline);
        free (cmdline);
        if (cline == NULL){                  /* Error in command line */
            continue;
		}	

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }
		
		for (struct list_elem * e = list_begin (&cline->pipes); 
		e != list_end (&cline->pipes); 
		e = list_next (e)) {
			struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
			run_pipeline(pipe);
		}
		
		/*for (struct list_elem * e = list_begin(&job_list); 
		e != list_end(&job_list); 
		e = list_next(e)) {
			struct job* j = list_entry(e, struct job, elem);
			if(j->num_processes_alive == 0){
				printf("here?\n");
				print_job(j);
				list_remove(&j->elem);
				delete_job(j);
			}
		}*/
		
		if(list_empty(&job_list) == false){
			struct list_elem* e = list_begin(&job_list);
			for(int i = 0; i < list_size(&job_list); i++){
				struct job* j = list_entry(e, struct job, elem);
				if(j->num_processes_alive == 0){
					list_remove(&j->elem);
					delete_job(j);
				}
				e = list_next(e);
			}
		}
		
		//ast_command_line_print(cline);
        //ast_command_line_free(cline);
    }
    return 0;
}

