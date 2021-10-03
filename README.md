# cs3214_p1
Student Information
-------------------
<Trevor McNeil - tmcneil2379>
<Alex Truxess - alexm00>
How to execute the shell:
------------------------
<describe how to execute from the command line>
    Find the src folder within the project, cd into it, run the make command to compile, then run ./cush
Important Notes
---------------
<Any important notes about your system>
Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >
    Begin by pulling arguments from the command line, and run matching command
    jobs:
        1. check if there are the proper number of arguments
        2. check if the job list is empty, if not iterate through it
        3. print all jobs
    fg:
        1. if there is one argument, then get most recently stopped job
        2. if there are two arguments, then get job that was specified
        3. move stopped job to foreground
    bg:
        1. if there is one argument, then get most recently stopped job
        2. if there are two arguments, then get job that was specified
        3. move stopped job to foreground
    kill:
        1. check if there are the proper number of arguments
        2. check for a valid job id
        3. remove the job
    stop: 
        1. check if there are the proper number of arguments
        2. check for a valid job id
        3. pause the job
    exit: exit the cush program
    \ˆC:
        Autohandled
    \ˆZ: 
        Autohandled
Description of Extended Functionality
-------------------------------------
<describe your IMPLEMENTATION of the following functionality:
I/O, Pipes, Exclusive Access >
List of Additional Builtins Implemented
---------------------------------------
(Written by Your Team)
<builtin name>
    custom_prompt
    prompt
<description>
    custom_prompt: you start off with a custom prompt that is "! \\u@\\h in \\W> ", which would output like 1 alexm00@hornbeam.rlogin in src> 
    prompt: this gives the user the ability to customize their prompt's PS1 variable. Options include:
        /u - username
        /h - hostname
        /w - current working directory
        /W - the basename of the current working directory
        /! - command number
        /n - new line
        /c - cush
        /d - current day in Year-Month-Day
        /T - current time in Hour:Minute:Second
        For user to customize their prompt, a user simply has to type 'prompt "insert your options here"'
