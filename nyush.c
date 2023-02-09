//
//  nyush.c
//  nyush
//
//  Created by Nikhil Korlipara on 10/24/22.
//
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <stdlib.h>
#include <fcntl.h>
#include <regex.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef enum { false=0, true=1} bool;

#define MAX_COMMANDLINE_LEN 100
#define ROOT_DIR "/"
#define PIPE "|"
#define SPACE " "
#define OUTPUT_REDIRECTION_1_STR ">"
#define OUTPUT_REDIRECTION_2_STR ">>"
#define INPUT_REDIRECTION_STR "<"
// exclude space (32), tab (9), > (62), < (60), | (124), * (42), ! (33), ` (96), ' (39), " (34)
//[a-zA-Z0-9_#\\$%&\\()+,\\-,\\./:;=?@\\[\\]\\^]+ #\\$\\^_

#define NAME_REGEX "^[a-zA-Z0-9_\\#\\$\\%\\&\\+\\,\\.\\/\\:\\;\\=\\?\\@~-]+$"

typedef enum { BUILT_IN_CMD_CD, BUILT_IN_CMD_JOBS, BUILT_IN_CMD_FG, BUILT_IN_CMD_EXIT, ARG, PROGRAM, INPUT_REDIRECTION, OUTPUT_REDIRECTION_1, OUTPUT_REDIRECTION_2, UNKNOWN } token_type;

typedef struct token {
    token_type tt;
    char* value;
} token_t;

/*
 * a command line will have one or more commands linked to each other
 * each command has a program path, list of args, infile_path, outfile_path, append_to_outfile_flag.
 */
typedef struct command {
    char* infile_path;
    char* outfile_path;
    bool append_to_outfile_flag;
    char* file;
    int argc;
    char* argv[];
} command_t;

#define MAX_JOBS 100
static int suspended_jobs_cnt = 0;

static regex_t name_regex;
static char cwd[1024];

static char* readLine(char* line, int len);
static void printPrompt();
static void printError(char* err);
static void installShellSignalHandlers();
static void installChildSignalHandlers();
static void executeCommandLine(char* command_line);
static command_t* buildCommand(char* command, bool input_redirect_allowed, bool output_redirect_allowed, bool built_in_command_allowed);
static char* trim(char* str);
static token_type getTokenType(char* str, bool is_first_token_of_command);
static bool isBuiltInCommand(token_type tt);
static int setCurrentDirectory();
static int changeDirectory(char* directory);
static void executeBuiltInCommand(token_type cmd_tt, char* tokens[]);
static int execute(command_t* cmds[], int cmd_cnt);


static char* BUILT_IN_PROGRAMS[] = { "cd", "exit", "jobs", "fg" };

/* terminate global variable is used to terminate the shell. */
bool terminate = false;

int main(int argc, char* argv[]) {
    char command_line[MAX_COMMANDLINE_LEN+1];
    int rc;
    if ((rc=regcomp(&name_regex, NAME_REGEX, REG_EXTENDED|REG_NOSUB)) != 0) {
        fprintf(stderr, "failed to compile regex for command name or command arg with error (%d). exiting", rc);
        exit(-1);
    }
    setCurrentDirectory();
    installShellSignalHandlers();
    do {
        printPrompt();
        /* read the next line */
        readLine(command_line, MAX_COMMANDLINE_LEN+1);
        
        if (strlen(command_line) == 0)
            continue;
        
        /* execute command_line */
        executeCommandLine(command_line);
        
     //   printf("command line read is: %s\n", command_line);
    } while (!terminate);
    
}


static bool fileExists(char* filename) {
       struct stat path_stat;
       stat(filename, &path_stat);
       return S_ISREG(path_stat.st_mode);
}

static int setCurrentDirectory() {
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        printError("getcwd(...) failed. unable to determine current dir");
        cwd[0] = '\0';
        return -1;
    }
    return 0;
}

// handle relative vs absolute dir changes
static int changeDirectory(char* dir) {
    if (0 == chdir(dir)) {
        return setCurrentDirectory();
    } else {
        fprintf(stderr, "Error: Invalid directory\n");
        return -1;
    }
}

/* ignore */
static void installShellSignalHandlers() {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
}

static void installChildSignalHandlers() {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
}

static void printError(char* err) {
    fprintf(stderr, "%s\n", err);
}

static void printPrompt() {
    char cwd_copy[1024];
    strcpy(cwd_copy, cwd);
    char* dirname = cwd_copy;
    char* ptr = cwd_copy;
    char* next = NULL;
    char* prev = NULL;
    char* saveptr;
    while (NULL != (next = strtok_r(ptr, "/", &saveptr))) {
        ptr = NULL;
        prev = next;
    }
    if (prev == NULL) {
        dirname = ROOT_DIR;
    } else {
        dirname = prev;
    }
    printf("[nyush %s]$ ", dirname);
    fflush(stdout);
}

/* remove trailing and leading whitespace chars */
static char* trim(char* str) {
    int len = strlen(str);
    if (len == 0)
        return str;
    while (*str != '\0' && isspace(*str))
        str++;
    len = strlen(str);
    if (len == 0)
        return str;
    char* last = str + len - 1;
    while (last != str && isspace(*last))
        last--;
    last[1] = '\0';
    return str;
}


/* read upto max len-1 into line from stdin. terminates the line with NULL character.
 if the line read is longer than len-1, discard all characters from len to end of line.
 return pointer to the line */
static char* readLine(char* line, int len) {
    int cread = 0;
    int c;
    while (cread < (len-1) && (c=getc(stdin)) != '\n') {
        line[cread] = c;
        cread++;
    }
    if (c == '\n') {
        line[cread] = '\0';
    } else {
    /* exhaust the input buffer until endof line */
        while ((c=getc(stdin)) != '\n') {
        }
    }
    return line;
}

static int numCharOccurences(char* str, char c) {
    int i=0;
    while(*str) {
        if (str[i]==c)
            i++;
        str++;
    }
    return i;
}
/*
 * input is a command_line which has no trailing while spaces
 */
static void executeCommandLine(char* command_line) {
    command_line = trim(command_line);
    int len;
    if ((len=strlen(command_line)) == 0) {
        return;
    }
    char* ptr = command_line;
    char* next = NULL;
    char* saveptr = NULL;
    char* commands[500];
    command_t* cmds[500];
    int command_cnt = 0;
    // if commandLine starts or ends with a pipe return error
    if (*command_line == PIPE[0] || command_line[len-1] == PIPE[0]) {
        fprintf(stderr, "Error: Invalid command\n");
        return;
    }
    int expectedCommands = numCharOccurences(command_line, PIPE[0]) + 1;
    while ((next = strtok_r(ptr, PIPE, &saveptr)) != NULL) {
        ptr = NULL;
        
        commands[command_cnt++] = next;
    }
    if (expectedCommands != command_cnt) {
        fprintf(stderr, "Error: Invalid command\n");
        return;
    }
    bool built_in_cmd_allowed = (command_cnt == 1);
    int i;
    for(i=0; i<command_cnt; i++) {
        char command[MAX_COMMANDLINE_LEN+1];
        strcpy(command, commands[i]);
        bool input_redirect_allowed = (i == 0);
        bool output_redirect_allowed = (i+1 == command_cnt);
        command_t* cmd = buildCommand(command, input_redirect_allowed, output_redirect_allowed, built_in_cmd_allowed);
        if (cmd == NULL) {
            return;
        }
        cmds[i] = cmd;
    }
    
    execute(cmds, command_cnt);
    
    // cleanup cmds
    for(i=0; i<command_cnt; i++) {
        free(cmds[i]->file);
        free(cmds[i]);
        
        cmds[i] = NULL;
    }
}

static int execute(command_t* cmds[], int cmd_cnt) {
    printf("cmd_cnt=%d\n",cmd_cnt);
    int i;
    for(i=0; i<cmd_cnt; i++) {
        pid_t pid;
        if ((pid=fork()) == 0) {
    	    printf("cmd_cnt=%d 1\n",cmd_cnt);
            int ifd = 0;
            if (cmds[0]->infile_path) {
                printf("infile=%s\n",cmds[0]->infile_path);
                ifd = open(cmds[0]->infile_path, O_RDONLY);
                if (ifd < 0) {
                    printf("failed to open infile=%s\n",cmds[0]->infile_path);
                    return -1;
                }
            }
            
    	    printf("cmd_cnt=%d 2\n",cmd_cnt);
            int ofd = 1;
            if (cmds[cmd_cnt-1]->outfile_path) {
                printf("cmd_cnt-1=%d outfile=%s, append=%d\n", cmd_cnt-1, cmds[cmd_cnt-1]->outfile_path, cmds[cmd_cnt-1]->append_to_outfile_flag);
                int flags = cmds[cmd_cnt-1]->append_to_outfile_flag ? O_WRONLY|O_APPEND|O_CREAT  : O_WRONLY|O_CREAT|O_TRUNC;
                ofd = open(cmds[cmd_cnt-1]->outfile_path, flags, 0666);
                if (ofd < 0) {
                    printf("failed to open outfile=%s\n",cmds[cmd_cnt-1]->outfile_path);
                    return -1;
                }
            }
            printf("i am here = %s", cmds[i]->file);
            if (i==0 && ifd > 0) {
		close(0);
                dup2(ifd, 0);
		close(ifd);
            } 
	    if (i==cmd_cnt-1 && ofd > 1) {
		close(1);
                dup2(ofd, 1);
		close(ofd);
            }
            printf("calling exec for file = %s", cmds[i]->file);
	    int j;
            for(j=0; i<cmds[j]->argc; j++) {
                printf("exec argv[%d]=%s", j, cmds[i]->argv[j]);
            }
            execvp(cmds[i]->file, cmds[i]->argv);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        } else {
		printf("fork failed");
	}
    }
    return 0;
}

static command_t* buildCommand(char* command, bool input_redirect_allowed, bool output_redirect_allowed, bool built_in_command_allowed) {
    
    char* ptr = command;
    char* next = NULL;
    char* saveptr = NULL;
    char* tokens[500];
    char file[1024];
    token_type token_types[500];
    int token_cnt = 0;
    bool seen_input_redirect = false;
    bool seen_output_redirect = false;
    int input_redirect_idx = -1;
    int output_redirect_idx = -1;
    int argc;
    while ((next = strtok_r(ptr, SPACE, &saveptr)) != NULL) {
        ptr = NULL;
        
        token_type tt = getTokenType(next, token_cnt==0);

        if (tt == INPUT_REDIRECTION) {
            if (seen_input_redirect) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
            seen_input_redirect = true;
            input_redirect_idx = token_cnt;
        } else if (tt == OUTPUT_REDIRECTION_1 || tt == OUTPUT_REDIRECTION_2) {
            if (seen_output_redirect) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
            seen_output_redirect = true;
            output_redirect_idx = token_cnt;
        } else if (tt == UNKNOWN) {
            fprintf(stderr, "Error: Invalid command\n");
            return NULL;
        }
        
        tokens[token_cnt] = next;
        token_types[token_cnt] = tt;
        token_cnt++;
    }
    
    if (token_cnt == 0)
        return NULL;
    
    token_type cmd_tt = token_types[0];
    if (isBuiltInCommand(cmd_tt)) {
        if (!built_in_command_allowed) {
            fprintf(stderr, "Error: Invalid command\n");
            return NULL;
        }
        if (seen_input_redirect || seen_output_redirect) {
            fprintf(stderr, "Error: Invalid command\n");
            return NULL;
        }
        
        if (cmd_tt == BUILT_IN_CMD_CD || cmd_tt == BUILT_IN_CMD_FG) {
            if (token_cnt != 2) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
        } else if (cmd_tt == BUILT_IN_CMD_EXIT || cmd_tt == BUILT_IN_CMD_JOBS) {
            if (token_cnt != 1) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
        }
        
        executeBuiltInCommand(cmd_tt, tokens);
        return NULL;
    } else if (cmd_tt == ARG) {
        argc = token_cnt;
        if (!input_redirect_allowed && seen_input_redirect) {
            fprintf(stderr, "Error: Invalid command\n");
            return NULL;
        } else if (!output_redirect_allowed && seen_output_redirect) {
            fprintf(stderr, "Error: Invalid command\n");
            return NULL;
        }
        if (seen_input_redirect) {
            if (input_redirect_idx+1 >= token_cnt || token_types[input_redirect_idx+1] != ARG) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
            // check if file exists
            if (fileExists(tokens[input_redirect_idx+1]) == false) {
                fprintf(stderr, "Error: invalid file\n");
                return NULL;
            }
            argc -= 2;
        }
        if (seen_output_redirect) {
            if (output_redirect_idx+1 >= token_cnt || token_types[output_redirect_idx+1] != ARG) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
            argc -= 2;
        }
        if (seen_input_redirect && seen_output_redirect) {
            int smaller = input_redirect_idx < output_redirect_idx ?
            input_redirect_idx : output_redirect_idx;
            if (token_cnt !=  (smaller+4)) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
        } else if (seen_input_redirect || seen_output_redirect) {
            int idx = seen_input_redirect ? input_redirect_idx : output_redirect_idx;
            if (token_cnt !=  (idx+2)) {
                fprintf(stderr, "Error: Invalid command\n");
                return NULL;
            }
        }
        if (strchr(tokens[0], '/') == NULL) {
            strcpy(file, "/usr/bin/");
            strcat(file, tokens[0]);
            if (fileExists(file) == false) {
                fprintf(stderr, "Error: invalid file\n");
                return NULL;
            }
        } else {
            strcpy(file, tokens[0]);
        }
    } else {
        fprintf(stderr, "Error: Invalid command\n");
        return NULL;
    }
    
    int sz = sizeof(command_t) + (argc+1)*sizeof(char*);
    command_t *t = (command_t*) malloc(sizeof(command_t) + (argc+1)*sizeof(char*));
    memset(t, 0, sz);
    t->argc = argc;
    int i;
    for (i=0; i<argc; i++) {
        t->argv[i] = tokens[i];
    }
    t->argv[argc] = NULL;
    if (seen_input_redirect) {
        t->infile_path = tokens[input_redirect_idx+1];
    }
    if (seen_output_redirect) {
        t->outfile_path = tokens[output_redirect_idx+1];
        if (token_types[output_redirect_idx] == OUTPUT_REDIRECTION_2) {
            t->append_to_outfile_flag = true;
        } else {
            t->append_to_outfile_flag = false;
        }
    }
    t->file = (char*)malloc(strlen(file)+1);
    strcpy(t->file, file);
    return t;
}

static void executeBuiltInCommand(token_type cmd_tt, char* tokens[]) {
    switch (cmd_tt) {
        case BUILT_IN_CMD_CD:
            changeDirectory(tokens[1]);
            break;
        case BUILT_IN_CMD_FG:
            break;
        case BUILT_IN_CMD_EXIT:
            if (suspended_jobs_cnt == 0)
                exit(0);
            else {
                fprintf(stderr, "Error: there are suspended jobs\n");
            }
            break;
        case BUILT_IN_CMD_JOBS:
            break;
        default:
            break;
    }
}

static bool isBuiltInCommand(token_type tt) {
    return tt == BUILT_IN_CMD_CD ||
    tt == BUILT_IN_CMD_FG ||
    tt == BUILT_IN_CMD_EXIT ||
    tt == BUILT_IN_CMD_JOBS;
}

static token_type getTokenType(char* str, bool is_first_token_of_command) {
    if (0 == strcmp(str, OUTPUT_REDIRECTION_1_STR)) {
        return OUTPUT_REDIRECTION_1;
    } else if (0 == strcmp(str, OUTPUT_REDIRECTION_2_STR)) {
        return OUTPUT_REDIRECTION_2;
    } else if (0 == strcmp(str, INPUT_REDIRECTION_STR)) {
        return INPUT_REDIRECTION;
    }
    if (is_first_token_of_command) {
        if (0 == strcmp(str, "cd")) {
            return BUILT_IN_CMD_CD;
        } else if (0 == strcmp(str, "fg")) {
            return BUILT_IN_CMD_FG;
        } else if (0 == strcmp(str, "exit")) {
            return BUILT_IN_CMD_EXIT;
        } else if (0 == strcmp(str, "jobs")) {
            return BUILT_IN_CMD_JOBS;
        }
    }
    int rc;
    if ((rc = regexec(&name_regex, str, 0, NULL, 0)) == 0) {
           return ARG;
    }
    return UNKNOWN;
}

