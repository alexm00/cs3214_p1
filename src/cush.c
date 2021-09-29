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

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
static char custom_prompt[] = "/! /u@/h in /W";

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
build_prompt(int* com_num, char custom_prompt[]){
    (*com_num) += 1;
    bool slash_checker = false;
    char character;
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    for (int i = 0; i < strlen(custom_prompt); i++) {
        character = custom_prompt[i];
        if (slash_checker){
            if (character == 'u'){
                char *username;
                username = "USER";
                printf("%s", getenv(username));
            }
            else if (character == 'h'){
                char hostname[100];
                gethostname(hostname, 100);
                printf("%s", hostname);
            }
            else if (character == 'd'){
                printf("%d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            }
            else if (character == 'T'){
                printf("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            }
            else if (character == 'W'){
                char *directory;
                directory = "PWD";
                printf("%s", basename(getenv(directory)));
            }
            else if (character == 'w'){
                char *directory;
                directory = "PWD";
                printf("%s", getenv(directory));
            }
            else if (character == '!'){
                printf("%d", *com_num);
            }
            else if (character == 'n'){
                printf("\n");
            }
            else {
                printf("%s", &character);
                if (character == '/') {
                    printf("/");
                }
            }
            slash_checker = false;
        }
        else if (character == '/'){
            slash_checker = true;
        }
        else {
            printf("%s", &character);
        }
    }
    printf(" > ");
    return strdup("");
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

static void started_stop_job(int jid){
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
		return;
	}
	else if(pid != j->pid){
		return;
	}
	else{
		
		if(WIFEXITED(status) || WIFSIGNALED(status)){
			//check for num_process_alive???
			j->num_processes_alive -= 1;
			if(j->num_processes_alive == 0){
				if(j->status == FOREGROUND){
					termstate_give_terminal_back_to_shell();
				}
				delete_job(j);
			}
		}
		else if(WIFSTOPPED(status)){
			if(j->status == FOREGROUND){
				//save tty state
				termstate_save(&j->saved_tty_state);
				termstate_give_terminal_back_to_shell();
				j->status = STOPPED;
			}
			else{
				j->status = STOPPED;
				if(WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN){
					j->status = NEEDSTERMINAL;
				}
			}
			add_stopped_job(j->jid);
		}
	}
}

static void execute(struct ast_pipeline* pipeline){
	
	struct job* cur_job = add_job(pipeline);
				
	signal_block(SIGCHLD);
				
	int pid = fork();
	
	if(pid == 0){
		
		signal_unblock(SIGCHLD);
		
		int size = list_size(&pipeline->commands);
		
		int pipes[size+1][2];
		for(int i = 0; i < size+1; i++){
			pipe(pipes[i]);
		}
		
		//int READ_END = 0;
		//int WRITE_END = 1;
		
		//input file
		//read from input file and pipe directly into write end of pipe////////////////////////////////////////////
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
		for (struct list_elem * e = list_begin(&pipeline->commands); 
		e != list_end(&pipeline->commands); 
		e = list_next(e)) {
			struct ast_command* cmd = list_entry(e, struct ast_command, elem);
			pid = fork();
			
			if(pid == 0){
				
				//pipeline pipes
				for(int i = 0; i < size+1; i++){
					if(i == com_num){
						if(i == 0 && input_fd > 0){
							dup2(input_fd, STDIN_FILENO);
						}
						else{
							dup2(pipes[i][0], STDIN_FILENO);
						}
					}
					else if(i == (com_num + 1)){
						if(i == size && output_fd > 0){
							dup2(output_fd, STDOUT_FILENO);
						}
						else{
							dup2(pipes[i][1], STDOUT_FILENO);
						}
					}
					close(pipes[i][0]);
					close(pipes[i][1]);
				}
				execvp(*cmd->argv, cmd->argv);
			}
			cur_job->num_processes_alive += 1;
			com_num++;
		}
		
		signal_unblock(SIGCHLD);
		
		//parent pipes
		//leave following pipes open:
		//pipe[0][1] for writing to first cmd
		//pipe[size][0] for reading from last cmd
		for(int i = 0; i < size + 1; i++){
			if(i == 0){
				if(input_fd == -1){
					dup2(STDIN_FILENO, pipes[i][1]);
				}
				else{
					close(pipes[i][1]);
				}
				close(pipes[i][0]);
			}
			else if(i == size){
				if(output_fd == -1){
					dup2(STDOUT_FILENO, pipes[i][0]);
				}
				else{
					close(pipes[i][0]);
				}
				close(pipes[i][1]);
			}
			else{
				close(pipes[i][0]);
				close(pipes[i][1]);
			}
		}
		
		//close remainging fds
		if(input_fd > 0){
			close(input_fd);
		}
		else{
			close(pipes[0][1]);
		}
		
		if(output_fd > 0){
			close(output_fd);
		}
		else{
			close(pipes[size][0]);
		}
		
		while(cur_job->num_processes_alive > 0){
			int status, id;
			id = waitpid(-1, &status, WNOHANG);
			if(id){
				cur_job->num_processes_alive -= 1;
			}
		}
		
		//read from output pipe///////////////////////////////////////////////////////////////////////
		
		exit(0);
	}
	
	cur_job->num_processes_alive += 1;
	cur_job->pid = pid;
	setpgid(pid, 0);
	if(cur_job->status == FOREGROUND){
		termstate_give_terminal_to(&cur_job->saved_tty_state, cur_job->pid);
		wait_for_job(cur_job);
	}
	signal_unblock(SIGCHLD);
				
}

static void run_pipeline(struct ast_pipeline* pipe){
	
	//parse pipeline for command arguments, determine validity of built-in commands, and retrieve job number for appropriate builtins;
	struct ast_command* com = list_entry(list_begin(&pipe->commands), struct ast_command, elem);
	char** cmd_argv = com->argv;
	int argc = 0;
	char* cur = *cmd_argv;
	while(cur != NULL){
		argc++;
		cur = *(cmd_argv + argc);
	}
	
	char** trash = NULL;
	
	if(strcmp(*cmd_argv, "exit") == 0){
		//call method to clean up all jobs and pipelines left/////////////////////////////////
		exit(0);
	}
	else if(strcmp(*cmd_argv, "kill") == 0){
		if(argc < 2){
			//not enough arguments
		}
		else if(argc > 2){
			//too many arguments
		}
		else{
			struct job* j = get_job_from_jid((int)strtol(*(cmd_argv+1), trash, 10));
			if(j == NULL){
				//job not found
			}
			else{
				kill(j->pid, SIGTERM);
			}
		}
	}
	else if(strcmp(*cmd_argv, "stop") == 0){
		if(argc < 2){
			//not enough arguments
		}
		else if(argc > 2){
			//too many arguments
		}
		else{
			struct job* j = get_job_from_jid((int)strtol(*(cmd_argv+1), trash, 10));
			if(j == NULL){
				//job not found
			}
			else{
				kill(j->pid, SIGSTOP);
			}
		}
	}
	else if(strcmp(*cmd_argv, "jobs") == 0){
		if(argc > 1){
			//too many arguments
		}
		else if(argc < 1){
			//not enough arguments
		}
		else{
			for (struct list_elem * e3 = list_begin(&job_list); 
			e3 != list_end(&job_list); 
			e3 = list_next(e3)) {
				struct job* j = list_entry(e3, struct job, elem);
				print_job(j);
			}
		}
	}
	else if(strcmp(*cmd_argv, "fg") == 0){
		//check for an existing foreground job???////////////////////////////////////////////////
		if(argc < 1){
			//too few arguments
		}
		else if(argc > 2){
			//too many arguments
		}
		else{
			struct job* j = NULL;
			
			if(argc == 1){
				//get job from last stopped job
				if(num_stop_job > 0){
					j = get_job_from_jid(stopped_jobs[num_stop_job-1]);
				}
				else{
					//no stopped jobs
				}
			}
			else{
				j = get_job_from_jid((int)strtol(*(cmd_argv+1), trash, 10));
				if(j->status != STOPPED){
					//job is currently running
					return;
				}
			}
			
			if(j == NULL){
				//job not found
			}
			else{
				//set tty state
				started_stop_job(j->jid);
				termstate_give_terminal_to(&j->saved_tty_state, j->pid);
				kill(j->pid, SIGCONT);
				j->status = FOREGROUND;
				wait_for_job(j);
			}
		}
	}
	else if(strcmp(*cmd_argv, "bg") == 0){
		if(argc < 1){
			//too few arguments
		}
		else if(argc > 2){
			//too many arguments
		}
		else{
			struct job* j = NULL;
			
			if(argc == 1){
				//get job from last stopped job
				if(num_stop_job > 0){
					j = get_job_from_jid(stopped_jobs[num_stop_job-1]);
				}
				else{
					//no stopped jobs
				}
			}
			else{
				j = get_job_from_jid((int)strtol(*(cmd_argv+1), trash, 10));
				if(j->status != STOPPED){
					//job is currently running
					return;
				}
			}
			
			if(j == NULL){
				//job not found
			}
			else{
				started_stop_job(j->jid);
				kill(j->pid, SIGCONT);
				j->status = BACKGROUND;
			}
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

		//check job status///////////////////////////////////////////////////////////////////////////////////
		//check_jobs();
	
	
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
		else{
			for (struct list_elem * e = list_begin (&cline->pipes); 
			e != list_end (&cline->pipes); 
			e = list_next (e)) {
				struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
				
				run_pipeline(pipe);
		
			}
			
		}
		
		/////////////////////////////////

        ast_command_line_print(cline);
        ast_command_line_free(cline);
    }
    return 0;
}








