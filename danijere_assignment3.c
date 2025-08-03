#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/types.h>

#define MAX_CHARS 2048
#define MAX_ARGS 512
#define MAX_BGPIDS 100
pid_t bgPIDs[MAX_BGPIDS];
int bgCount = 0;
volatile sig_atomic_t foregroundOnly = 0;

void handle_SIGTSTP(int signo) {
    if (foregroundOnly == 0) {
        char* message = "\nEntering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, message, 53);
        foregroundOnly = 1;
    } else {
        char* message = "\nExiting foreground-only mode\n";
        write(STDOUT_FILENO, message, 32);
        foregroundOnly = 0;
    }
    fflush(stdout);
}

char* expand_pid(const char* input) {
    static char buffer[MAX_CHARS + 1];
    char temp[MAX_CHARS + 1];
    buffer[0] = '\0';
    const char* p = input;
    while (*p) {
        if (*p == '$' && *(p + 1) == '$') {
            snprintf(temp, sizeof(temp), "%d", getpid());
            strncat(buffer, temp, sizeof(buffer) - strlen(buffer) - 1);
            p += 2;
        } else {
            strncat(buffer, p, 1);
            p++;
        }
    }
    return buffer;
}

int main () {
    char input[MAX_CHARS + 1];
    int lastStatus = 0;

    struct sigaction SIGINT_action = {0};
    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;
    sigaction(SIGINT, &SIGINT_action, NULL);

    struct sigaction SIGTSTP_action = {0};
    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    while (1) {
        for (int i = 0; i < bgCount; i++) {
            int childStatus;
            pid_t result = waitpid(bgPIDs[i], &childStatus, WNOHANG);
            if (result > 0) {
                if (WIFEXITED(childStatus)) {
                    printf("background pid %d is done: exit value %d\n", result, WEXITSTATUS(childStatus));
                } else if (WIFSIGNALED(childStatus)) {
                    printf("background pid %d is done: terminated by signal %d\n", result, WTERMSIG(childStatus));
                }
                fflush(stdout);
                for (int j = i; j < bgCount - 1; j++) {
                    bgPIDs[j] = bgPIDs[j + 1];
                }
                bgCount--;
                i--;
            }
        }

        printf(": ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            clearerr(stdin);
            continue;
        }

        if (input[0] == '\n' || input[0] == '#') {
            continue;
        }

        input[strcspn(input, "\n")] = 0;
        char* expandedInput = expand_pid(input);

        char *args[MAX_ARGS];
        int argc = 0;
        int isBackground = 0;
        char *token = strtok(expandedInput, " ");
        while (token != NULL && argc < MAX_ARGS - 1) {
            args[argc++] = token;
            token = strtok(NULL, " ");
        }
        args[argc] = NULL;

        if (argc > 0 && strcmp(args[argc - 1], "&") == 0) {
            if (foregroundOnly == 0) {
                isBackground = 1;
            }
            args[--argc] = NULL;
        }

        if (args[0] == NULL) {
            continue;
        }

        if (strcmp(args[0], "exit") == 0) {
            for (int i = 0; i < bgCount; i++) {
                kill(bgPIDs[i], SIGTERM);
            }
            break;
        } else if (strcmp(args[0], "cd") == 0) {
            char *target = args[1];
            if (target == NULL) {
                target = getenv("HOME");
            }
            if (chdir(target) != 0) {
                perror("cd");
            }
        } else if (strcmp(args[0], "status") == 0) {
            printf("exit value %d\n", lastStatus);
            fflush(stdout);
        } else {
            pid_t spawnpid = fork();
            if (spawnpid == -1) {
                perror("fork");
                continue;
            } else if (spawnpid == 0) {
                int inputRedirect = -1;
                int outputRedirect = -1;

                if (isBackground && inputRedirect == -1) {
                    inputRedirect = open("/dev/null", O_RDONLY);
                    dup2(inputRedirect, 0);
                    close(inputRedirect);
                }

                if (isBackground && outputRedirect == -1) {
                    outputRedirect = open("/dev/null", O_WRONLY);
                    dup2(outputRedirect, 1);
                    close(outputRedirect);
                }

                for (int i = 0; args[i] != NULL; i++) {
                    if (strcmp(args[i], "<") == 0 && args[i + 1] != NULL) {
                        inputRedirect = open(args[i + 1], O_RDONLY);
                        if (inputRedirect == -1) {
                            perror(args[i + 1]);
                            exit(1);
                        }
                        dup2(inputRedirect, 0);
                        close(inputRedirect);
                        args[i] = NULL;
                        args[i + 1] = NULL;
                    } else if (strcmp(args[i], ">") == 0 && args[i + 1] != NULL) {
                        outputRedirect = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (outputRedirect == -1) {
                            perror(args[i + 1]);
                            exit(1);
                        }
                        dup2(outputRedirect, 1);
                        close(outputRedirect);
                        args[i] = NULL;
                        args[i + 1] = NULL;
                    }
                }

                SIGINT_action.sa_handler = SIG_DFL;
                sigfillset(&SIGINT_action.sa_mask);
                SIGINT_action.sa_flags = 0;
                sigaction(SIGINT, &SIGINT_action, NULL);

                execvp(args[0], args);
                perror(args[0]);
                exit(1);
            } else {
                int childStatus;
                if (isBackground) {
                    printf("background pid is %d\n", spawnpid);
                    if (bgCount < MAX_BGPIDS) {
                        bgPIDs[bgCount++] = spawnpid;
                    }
                    fflush(stdout);
                } else {
                    waitpid(spawnpid, &childStatus, 0);
                    if (WIFEXITED(childStatus)) {
                        lastStatus = WEXITSTATUS(childStatus);
                    } else if (WIFSIGNALED(childStatus)) {
                        lastStatus = WTERMSIG(childStatus);
                        printf("terminated by signal %d\n", lastStatus);
                        fflush(stdout);
                    }
                }
            }
        }
    }

    return 0;
}
