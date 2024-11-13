#define _GNU_SOURCE 

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "list.h"
#include "mu.h"

#define USAGE \
    \
    "Usage bsh [-h] \n" \
    "\n" \
    "bsh is a basic shell implementing redirection of stdin and stdout, and pipelines\n" \
    "\n" \
    "-h, --help \n" \
    "\tPrint a usage statement to stdout and exit with status 0\n"



#define CMD_INITIAL_CAP_ARGS 8

int last_exit_status = 0;

void last_error() {
    printf("%d\n", last_exit_status);
}

static void usage(int status) {
    puts(USAGE);
    exit(status);
}

struct cmd {
    struct list_head list;

    char **args;
    size_t num_args; // the number of args in the array
    size_t cap_args; // capacity of the args array

    pid_t pid;
};

struct pipeline {
    struct list_head head;  /* cmds */
    size_t num_cmds;
    char *in_file;
    char *out_file;
    bool append;
};


static struct cmd *
cmd_new(void)
{
    MU_NEW(cmd, cmd);
    size_t i;

    cmd->cap_args = CMD_INITIAL_CAP_ARGS; // this will expand if we have more than 8
    cmd->args = mu_mallocarray(cmd->cap_args, sizeof(char *));

    for (i = 0; i < cmd->cap_args; i++)
        cmd->args[i] = NULL;

    return cmd;
}


static void
cmd_push_arg(struct cmd *cmd, const char *arg)
{
    if (cmd->num_args == cmd->cap_args) {
        cmd->args = mu_reallocarray(cmd->args, cmd->cap_args * 2, sizeof(char *));
        cmd->cap_args *= 2;
    }

    cmd->args[cmd->num_args] = mu_strdup(arg); 
    cmd->num_args += 1;
}


static void
cmd_pop_arg(struct cmd *cmd)
{
    assert(cmd->num_args > 0);

    free(cmd->args[cmd->num_args - 1]);
    cmd->args[cmd->num_args - 1] = NULL;

    cmd->num_args--;
}


static void
cmd_free(struct cmd *cmd)
{
    size_t i;

    for (i = 0; i < cmd->num_args; i++)
        free(cmd->args[i]);

    free(cmd->args);
    free(cmd);
}


static void
cmd_print(const struct cmd *cmd)
{
    size_t i;

    printf("cmd {num_args:%zu, cap_args:%zu}:\n",
            cmd->num_args, cmd->cap_args);
    for (i = 0; i < cmd->num_args; i++)
        printf("\t[%zu] = \"%s\"\n", i, cmd->args[i]);
}


static struct pipeline *
pipeline_new(char *line)
{
    MU_NEW(pipeline, pipeline);
    struct cmd *cmd = NULL;
    char *s1, *s2, *command, *arg;
    char *saveptr1, *saveptr2;
    int i;

    INIT_LIST_HEAD(&pipeline->head);

    for (i = 0, s1 = line; ; i++, s1 = NULL) {
        /* break into commands */
        command = strtok_r(s1, "|", &saveptr1);
        if (command == NULL)
            break;

        cmd = cmd_new();

        /* parse the args of a single command */
        for (s2 = command; ; s2 = NULL) {
            arg = strtok_r(s2, " \t", &saveptr2);
            if (arg == NULL)
                break;
            cmd_push_arg(cmd, arg);
        }
        list_add_tail(&cmd->list, &pipeline->head);
        pipeline->num_cmds += 1;
    }

    /* TODO: parse I/O redirects */
    list_for_each_entry(cmd, &pipeline->head, list){
        if (cmd != NULL /*&& cmd->num_args > 1*/)  {
            for (size_t x = 0; x < cmd->num_args; x++) {
                switch (cmd->args[x][0]) {

                    case '<':
                        if (cmd->args[x][1] != '\0') { // checking to see if there is a file name after redir char
                            pipeline->in_file = (cmd->args[x] + 1);
                            cmd->args[x] = '\0';
                        } else {
                            fprintf(stderr, "No input file name detected\n");
                        }
                        break;

                    case '>': // output redirection
                        if (cmd->args[x][1] != '\0'){
                            if (cmd->args[x][1] == '>' && cmd->args[x][2] != '\0'){ // append char
                                pipeline->out_file = (cmd->args[x] + 2);
                                pipeline->append = true;
                            } else {
                                pipeline->out_file = (cmd->args[x] + 1);
                            }
                        } else {
                            fprintf(stderr, "No output file name detected\n");
                        }
                        cmd->args[x] = '\0';
                        break;

                    default:
                        break;
                }
            }
        }
    }
    return pipeline;
}


static void
pipeline_free(struct pipeline *pipeline)
{
    struct cmd *cmd, *tmp;

    list_for_each_entry_safe(cmd, tmp, &pipeline->head, list) {
        list_del(&cmd->list);
        cmd_free(cmd);
    }

    free(pipeline);
}


static void
pipeline_print(const struct pipeline *pipeline)
{
    struct cmd *cmd;
    list_for_each_entry(cmd, &pipeline->head, list) {
        cmd_print(cmd);
    }
}

/* iterates through linked list of commands, waits on each command to see how 
 * it terminated:
 * nomal case - we want exit status of the command
 * abnormal case - we want to know which signal killed it 
*/
static int
pipeline_wait_all(const struct pipeline *pipeline) {
    struct cmd *cmd;
    pid_t pid;
    int wstatus;
    int exit_status = 0;
    list_for_each_entry(cmd, &pipeline->head, list) {
        pid = waitpid(cmd->pid, &wstatus, 0);

        if (pid == -1) {
            mu_die_errno(errno, "waitpid(%d) failed",pid);
        }
        
        if (WIFEXITED(wstatus)) // true or false if process terminated normally
            exit_status = WEXITSTATUS(wstatus);
        
        else if (WIFSIGNALED(wstatus))
            exit_status = 128 + WTERMSIG(wstatus);        

    }
    // fprintf(stderr, "WAIT");
    return exit_status;
}


static int 
pipeline_eval(struct pipeline *pipeline) {
    struct cmd *cmd;
    pid_t pid;
    int exit_status = 0;

    /* pipe variable instantiation */
    int err; 
    int pfd[2];
    size_t cmd_idx = 0;
    bool created_pipe = false;
    int wfd;
    int prev_rfd;
    struct cmd *first_cmd = list_first_entry(&pipeline->head, struct cmd, list);
    if (strcmp(first_cmd->args[0], "last_error") == 0) {
        last_error();
    }
    else { 
        list_for_each_entry(cmd, &pipeline->head, list) {

            // fprintf(stderr, "in_file is: %s\nout_file is: %s\n" , pipeline->in_file, pipeline->out_file);

            if ((pipeline->num_cmds > 1) && (cmd_idx != pipeline->num_cmds-1)) {
            /* section of code responsible for invoking a pipe*/
                err = pipe(pfd);
                if (err != 0)
                    mu_die_errno(errno, "pipe");
                created_pipe = true;
                
            }
        
            pid = fork();
            if (pid == -1) {
                mu_die_errno(errno, "fork");
            }

            if (pid == 0) {
                /* CHILD */

                /* adjust stdout */
                if (cmd_idx == pipeline->num_cmds - 1) {
                    wfd = STDOUT_FILENO; // if it is the last command, we want the output to go to stdout
                    if (pipeline->out_file != NULL) {
                        /* changing stdout to some file */
                        int out_fd;
                        if (pipeline->append) {
                            out_fd = open(pipeline->out_file, O_CREAT | O_WRONLY | O_APPEND, 0666);
                        } else {
                            out_fd = open(pipeline->out_file, O_CREAT | O_WRONLY, 0666);
                        }
                        dup2(out_fd, STDOUT_FILENO);
                        close(out_fd);
                    }
                    
                } else {
                    wfd = pfd[1];
                    dup2(wfd, STDOUT_FILENO);
                    close(wfd);
                }
                
                /* adjust stdin*/            
                if (cmd_idx > 0) {
                    dup2(prev_rfd, STDIN_FILENO);
                    close(prev_rfd);
                } else {
                    if (pipeline->in_file != NULL) {
                        int in_fd = open(pipeline->in_file, O_RDONLY);
                        dup2(in_fd, STDIN_FILENO);
                        close(in_fd);
                    }
                }

                /* exec command */
                execvp(cmd->args[0], cmd->args); 
                mu_die_errno(errno, "can't exec %s", cmd->args[0]); 
                
            }
            /* PARENT */
            cmd->pid = pid; // adding the pid to pid cmd field for each entry

            if (created_pipe && (cmd_idx != pipeline->num_cmds-1)) {
                err = close(pfd[1]);
                if (err == -1)
                    mu_die_errno(errno, "parent failed to close write end of pipe");
            }
            if (cmd_idx > 0) {
                close(prev_rfd);
            }
            /* note here, don't have to worry about parent overwriting
            * the child's prev_rfd as the child gets a copy of 
            * the file descriptor*/
            prev_rfd = pfd[0]; 
            cmd_idx++;
        }
        exit_status = pipeline_wait_all(pipeline);
    }
    return exit_status;
}


int
main(int argc, char *argv[])
{
    ssize_t len_ret = 0;
    size_t len = 0;
    char *line = NULL;
    struct pipeline *pipeline = NULL;

    /* TODO: getopt_long */

    int opt, nargs;
    MU_UNUSED(nargs);

    const char *short_opts = "h"; // this has to be all of characters for the possible short options - opt returns \?
    struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    while (1) {
        opt = getopt_long(argc, argv, short_opts, long_opts, NULL); // printf("opt: %d, character is %c\n\n", opt, opt);
        if (opt == -1) { // getopt_long returns -1 after parsing all options
            break;
        }

        switch (opt) {
            
            case 'h' : usage(0); break;

            case '?':
                mu_die("unknown option '%c' (decimal: %d)", optopt, optopt);
                break;
            case ':':
                mu_die("missing option argument for option %c", optopt);
                break;
            default:
                mu_die("unexpected getopt_long return value: %c", (char)opt);
                break;
            
        
        }
    }
    MU_UNUSED(argc);
    MU_UNUSED(argv);

    /* REPL */
    while (1) {
        if (isatty(fileno(stdin)))
            printf("> ");
        len_ret = getline(&line, &len, stdin);
        if (len_ret == -1)
            goto out;
        
        mu_str_chomp(line);
        pipeline = pipeline_new(line);

        
        /* TODO eval */
        last_exit_status = pipeline_eval(pipeline);

        pipeline_free(pipeline);
    }

out:
    free(line);
    return 0;
}