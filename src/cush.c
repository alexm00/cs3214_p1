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

static void handle_child_status(pid_t pid, int status);

static char* custom_prompt = "\\! \\u@\\h in \\W> ";

static void
usage(char *progname){
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/*prints a custom prompt that the suer specifies*/
static char*
build_prompt(int* com_num){
	
	//variables needed for parsing and displaying the custom_prompt
    (*com_num) += 1; //increment the command number (for use of '!')
	int prompt_size = strlen(custom_prompt); //get how many characters are in the custom prompt
	char cur_char = *custom_prompt; //variable to loop through the custom_prompt
	time_t t = time(NULL); //time object for the date and time functionality
	bool special = false; //bool to determine if a backslash is used in the prompt
	int count = 0; //keeps track of the character position in the custom prompt
	
	//loop through custom_prmpt
	while(count < prompt_size){
		
		//if a backslash was detected, run the special characters
		if(special){
			switch(cur_char){ //switch case for characters following a backslash
				case 'u': //prints the user
					printf("%s", getenv("USER"));
					break;
				case 'h': ; //prints the hostname
					char host_field[33];
					gethostname(host_field, 32);
					host_field[32] = '\0';
					printf("%s", host_field);
					break;
				case 'w': //prints the entire working directory path
					printf("%s", getenv("PWD"));
					break;
				case 'W': //pints only the current directory name (not the full path)
					printf("%s", basename(getenv("PWD")));
					break;
				case 'd': ;//displays the date
					struct tm tm = *localtime(&t);
					printf("%02d-%02d-%d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
					break;
				case 'T': ;//displays the time
					struct tm tm2 = *localtime(&t);
					printf("%02d:%02d", tm2.tm_hour, tm2.tm_min);
					break;
				case 'n': //new line character
					printf("\n");
					break;
				case 'c': //prints the name of the program 'cush'
					printf("cush");
					break;
				case '!': //incase a user wants to actually use an '!' they can just add a slash to it
					printf("%d", *com_num);
					break;
				default: //if the character following the slash isn't a special character
					printf("\\");
					printf("%c", cur_char);
					break;
			}
			special = false; //reset the special character boolean
		}
		else if(cur_char == '\\'){ //if a slash is detected, check next character to determine cpecial character output, if any
			special = true;
		}
		else{ //otherwise print the current character
			printf("%c", cur_char);
		}
		count++; //increase character position
		cur_char = *(custom_prompt + count); //get next character
	}
	return strdup(""); //return
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

//variables for storing stopped jobs
static int stopped_jobs[MAXJOBS]; //stores the stopped jobs in an array, with the highest non-zero index being the moost recently stopped job
static int num_stop_job = 0; //stores the number of stopped jobs, used for indexing the array for only valid indexes

/*adds a job to the stopped_jobs array*/
static void add_stopped_job(int jid){
	stopped_jobs[num_stop_job] = jid; //places jid in highest array index
	num_stop_job++; //increments counter
}

/*removes a job from the stopped_jobs array*/
static void start_stopped_job(int jid){
	bool started = false; //if found the stopped job referencing the jid passed in
	
	//loops through job array
	for(int i = 0; i < num_stop_job; i++){
		//if jid found, set boolean to true
		if(!started && jid == stopped_jobs[i]){
			started = true;
		}
		//if jid found in array, shift over all other jids in array
		if(started){
			if(i < num_stop_job-1){
				stopped_jobs[i] = stopped_jobs[i+1];
			}
			else if(i == num_stop_job - 1){
				stopped_jobs[i] = 0; //set final index to 0
			}
		}
	}
	if(started){ //if jid found in array, decrement counter
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
	job->pid = 0;
	
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
	
	//int pgid = getpgid(pid);
	
	//make sure pid is a valid pid
	if(pid > 0){
		struct job* j = NULL; //job variable
		//loops through job list to fid job that refers to pid
		for (struct list_elem * e = list_begin(&job_list); 
		e != list_end(&job_list); 
		e = list_next(e)) {
			j = list_entry(e, struct job, elem);
			if(j->pid == pid){ //if job matches pid, break out of loop
				break;
			}
			j = NULL;
		}
	
		if(j == NULL){ //if no job was found with the following pid
			//return; //??? No error???
			utils_fatal_error("Error: There are currently no known jobs to recieve signal from");
		}
		//else if(pid != j->pid){ //if the pid doesn't match the pid of the job
			//utils_fatal_error("Error: No job was found matching the ended process pid");
		//}
		else{
			if(WIFEXITED(status)){ //test if the program exited
				j->num_processes_alive--; //decrement processes counter for job
			}
			else if(WIFSIGNALED(status)){ //test if the program was terminated with a signal, send error message based on signal recieved
				int termsig = WTERMSIG(status);
				if (termsig == 6) { //aborted signal
					utils_error("aborted\n");
				}
				else if (termsig == 8) { //floating point exception signal
					utils_error("floating point exception\n");
				}
				else if (termsig == 9) { //killed signal
					utils_error("killed\n");
				}
				else if (termsig == 11) { //segmentation fault signal
					utils_error("segmentation fault\n");
				}
				else if (termsig == 15) { //terminated signal
					utils_error("terminated\n");
				}
				j->num_processes_alive--; //decrement processes counter for job
			}
			else if(WIFSTOPPED(status)){ //test if job was stopped
				j->status = STOPPED; //set stopped status
				int stop_sig = WSTOPSIG(status); //get the specific stopped signal
				//test if program was a foreground command to save terminal state
				if(j->status == FOREGROUND){
					termstate_save(&j->saved_tty_state); //save tty state
					print_job(j);
				}
				else{ //runs if job was in the background
					if(stop_sig == SIGTTOU || stop_sig == SIGTTIN){ //tests if the job was stoped do to needing terminal access
						j->status = NEEDSTERMINAL;
					}
					else{
						print_job(j);
					}
				}
				add_stopped_job(j->jid); //add job to stopped_job array
			}
			termstate_give_terminal_back_to_shell(); //return termianl access back to shell
		}
	}
	else{ //error if pid is invalid
		utils_fatal_error("Error in waiting for signal from child process");
	}
}

static void execute(struct ast_pipeline* pipeline){
	
	//make job from pipeline
	struct job* cur_job = add_job(pipeline);
				
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
		
		//split to child and parent processes
		pid = fork(); 
			
		if(pid == 0){
				
			//child pipes
			for(int i = 0; i <= size; i++){
				if(i == com_num){
					if(i == 0){
						if(input_fd > 0){
							//dup2(pipes[i][0], STDIN_FILENO);
							dup2(input_fd, STDIN_FILENO);
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
			
			//assign stderr to stdout
			if(cmd->dup_stderr_to_stdout){
				dup2(STDOUT_FILENO, STDERR_FILENO);
			}
				
			//execute
			execvp(*cmd->argv, cmd->argv);
			
			//if execute failed
			printf("no such file or directory\n");
			exit(0);
		}
		
		//assign job pid
		if(cur_job->pid == 0){
			cur_job->pid = pid;
		}
		//set child process pgid
		setpgid(pid, cur_job->pid);
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
		else{
			close(pipes[i][0]);
			close(pipes[i][1]);
		}
	}
		
	/*//read from file to pipe
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
	*/
	//close remainging output fds
	if(output_fd > 0){
		close(output_fd);
	}
	if(input_fd > 0){
		close(output_fd);
	}
		
	//save good terminal state
	termstate_save(&cur_job->saved_tty_state);
	
	//if job is foreground
	if(cur_job->status == FOREGROUND){
		termstate_give_terminal_to(&cur_job->saved_tty_state, cur_job->pid);
		wait_for_job(cur_job);
	}
	//if job is background
	else if(cur_job->status == BACKGROUND){
		printf("[%d] %d\n", cur_job->jid, cur_job->pid);
	}
	
	//give terminal back to shell
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
	
	if(strcmp(*cmd_argv, "exit") == 0){ //exit nuilt-in
		exit(0);
	}
	else if(strcmp(*cmd_argv, "kill") == 0){ //kill built-in
		if(argc == 2){ //test for correct number of arguments
			int jid = atoi(*(cmd_argv + 1)); //convert jid from argv to int
			struct job* j = get_job_from_jid(jid); //retrieve job from jid
			if(j == NULL){ //error if job was not found
				printf("jid: %d was not found among the current jobs\n", jid);
			}
			else{ //if job was found
				int ret_status = killpg(j->pid, SIGTERM); //set signal
				if(ret_status != 0){
					list_remove(&j->elem);
				}
				if(ret_status < 0){ //signal failure
					printf("Kill on job: %d was unsuccessful\n", jid);
				}
			}
		}
		else{ //error if incorrect number of arguments
			printf("Incorrect number of arguments for command 'kill'\n");
		}
	}
	else if(strcmp(*cmd_argv, "stop") == 0){ //stop built-in
		if(argc == 2){ //test for correct number of arguments
			int jid = atoi(*(cmd_argv + 1)); //convert jid from argv to int
			struct job* j = get_job_from_jid(jid); //retrieve job from jid
			if(j == NULL){ //error if job was not found
				printf("jid: %d was not found among the current jobs\n", jid);
			}
			else{ //if job was found
				int ret_status = killpg(j->pid, SIGSTOP); //send signal
				if(ret_status >= 0){ //signal success
					j->status = STOPPED; //set status here?
					termstate_save(&j->saved_tty_state);
				}
				else{ //signal failure
					printf("Stop on job: %d was unsuccessful\n", jid);
				}
			}
		}
		else{ //error if incorrect number of arguments
			printf("Incorrect number of arguments for command 'stop'\n");
		}
	}
	else if(strcmp(*cmd_argv, "jobs") == 0){ //jobs built-in
		if(argc == 1){ //test for correct number of arguments
			if(!list_empty(&job_list)){ //if job list is not empty
				//loop through job list
				for (struct list_elem * e = list_begin(&job_list); 
				e != list_end(&job_list); 
				e = list_next(e)) {
					struct job* j = list_entry(e, struct job, elem);
					print_job(j); //print jobs
				}
			}
			else{ //error if job list is empty
				printf("There are currently no jobs\n");
			}
		}
		else{ //error if incorrect number of arguments
			printf("Incorrect number of arguments for command 'jobs'\n");
		}
	}
	else if(strcmp(*cmd_argv, "fg") == 0){ //fg built-in
		
		//job variables
		struct job* j = NULL;
		int jid = 0;
		
		if(argc == 1){ //if 1 argument 'fg'
			//get job from last stopped job
			if(num_stop_job > 0){ //if there is atleat 1 stopped job, retrieve it
				j = get_job_from_jid(stopped_jobs[num_stop_job-1]);
				jid = j->jid;
			}
			else{ //no stopped jobs
				printf("There are currently no stopped jobs\n");
				return;
			}
		}
		else if(argc == 2){ //if 2 arguments 'fg [jid]'
			jid = atoi(*(cmd_argv + 1)); //retrieve jid
			j = get_job_from_jid(jid); //get job from jid
			if(j == NULL){ //if job wasn't found
				printf("No job matching jid\n");
				return;
			}
			if(j->status == FOREGROUND){ //if already foreground, print message
				printf("Job: %d is already running\n", jid);
				return;
			}
		}
		else{ //error for incorrect number of arguments
			printf("Incorrect number of arguments for command 'fg'\n");
			return;
		}
		
		if(j == NULL){ //error if no job found
			printf("There was no stopped job found\n");
			return;
		}
		else{ //if job found
			signal_block(SIGCHLD); //block signal
			int ret_status = killpg(j->pid, SIGCONT); //send continue signal
			if(ret_status >= 0){ //signal success
				start_stopped_job(j->jid); //remove jid from stopped_jobs array
				termstate_give_terminal_to(&j->saved_tty_state, j->pid); //give terminal to job
				j->status = FOREGROUND; //set job status to foreground
				print_job(j); //print job
				wait_for_job(j); //wait for job completion
			}
			else{ //signal failure
				printf("fg on job: %d was unsuccessful\n", jid);
			}
			signal_unblock(SIGCHLD); //unblock signal
			termstate_give_terminal_back_to_shell(); //return terminal to shell
		}
		
	}
	else if(strcmp(*cmd_argv, "bg") == 0){ //bg built-in
		
		//job variables
		struct job* j = NULL;
		int jid = 0;
		
		if(argc == 1){ //if 1 argument 'bg'
			//get job from last stopped job
			if(num_stop_job > 0){ //if there is atleat 1 stopped job, retrieve it
				j = get_job_from_jid(stopped_jobs[num_stop_job-1]);
				jid = j->jid;
			}
			else{ //no stopped jobs
				printf("There are currently no stopped jobs\n");
				return;
			}
		}
		else if(argc == 2){ //if 2 arguments 'bg [jid]'
			jid = atoi(*(cmd_argv + 1)); //retrieve jid
			j = get_job_from_jid(jid); //get job from jid
			if(j == NULL){ //if job wasn't found
				printf("No job matching jid\n");
				return;
			}
			else if(j->status != STOPPED){ //if job is already running
				printf("Job: %d is already running\n", jid);
				return;
			}
		}
		else{ //error for incorrect number of arguments
			printf("Incorrect number of arguments for command 'bg'\n");
			return;
		}
			
		if(j == NULL){ //if job was found
			printf("There are currently no stopped jobs\n");
			return;
		}
		else{ //if job found
			int ret_status = killpg(j->pid, SIGCONT); //send continue signal
			if(ret_status >= 0){ //signal success
				start_stopped_job(j->jid); //remove job from stopped jobs array
				j->status = BACKGROUND; //set background status
				print_job(j); //print job
			}
			else{ //signal failure
				printf("bg on job: %d was unsuccessful\n", jid);
			}
			termstate_give_terminal_back_to_shell(); //give terminal back to shell
		}
	}
	else if(strcmp(*cmd_argv, "prompt") == 0){ //custom prompt built-in
		if(argc == 1){ //if 1 argument, print current prompt format
			printf("The current prompt expression is: \'%s\'\n", custom_prompt);
		}
		else if(argc == 2){ //if 2 arguments, set prompt passed in format
			custom_prompt = *(cmd_argv + 1);
			printf("Set the prompt expression to: \'%s\'\n", custom_prompt);
		}
		else{ //if incorrect arguments to prompt
			printf("Incorrect number of arguments for command 'prompt'\n");
		}
	}
	else{ //execute other program
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
		
		//loop through pipelines in the command line
		for (struct list_elem * e = list_begin (&cline->pipes); 
		e != list_end (&cline->pipes); 
		e = list_next (e)) {
			struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
			run_pipeline(pipe); //send pipeline to get processed
		}
		
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

