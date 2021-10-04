# cs3214_p1
Student Information
-------------------
<Trevor McNeil - tmcneil2379>
<Alex Truxess - alexm00>

How to execute the shell:
------------------------
Find the src folder within the project, cd into it, run the make command to compile, then run ./cush

Important Notes
---------------
None

Description of Base Functionality
---------------------------------
    Begin by pulling arguments from the command line, and run matching command
    jobs:
        1. check if there are the proper number of arguments
        2. check if the job list is empty, if not iterate through it
        3. print all jobs
    fg:
        1. if there is one argument, then get most recently stopped job
        2. if there are two arguments, then get job that was specified
        3. move stopped or background job to foreground
    bg:
        1. if there is one argument, then get most recently stopped job
        2. if there are two arguments, then get job that was specified
        3. move stopped job to background
    kill:
        1. check if there are the proper number of arguments
        2. check for a valid job id
		3. send kill signal
    stop: 
        1. check if there are the proper number of arguments
        2. check for a valid job id
		3. set stop signal
        4. save terminal state
    exit: 
		1. exit the cush program
    \ˆC:
        Autohandled as kill signal
    \ˆZ: 
        Autohandled as stop signal
		
Description of Extended Functionality
-------------------------------------
1. the first command gets access to stdin, or input file descriptor
2. the commands inbetween are piped such that output from 
   the first goes to the input of the next, until the final 
   command
3. the last command gets access to stdout, or output file descriptor

exclusive acces is handles by a single command has acces to bout stdin and stdout


List of Additional Builtins Implemented
---------------------------------------
(Written by Your Team)
<builtin name>
    custom_prompt
    prompt
<description>
    custom_prompt: you start off with a custom prompt that is "\\! \\u@\\h in \\W> ", which would output like 1 alexm00@hornbeam.rlogin in src> 
    prompt: this gives the user the ability to customize their prompt's PS1 variable. Options include:
        \u - username
        \h - hostname
        \w - current working directory
        \W - the basename of the current working directory
        \! - command number
        \n - new line
        \c - cush
        \d - current day in Month-Day-Year
        \T - current time in Hour:Minute
        For user to customize their prompt, a user simply has to type 'prompt "insert your options here"'
		Make sure that the custom propmt you are setting is surrounded by ""
