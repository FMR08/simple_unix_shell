#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define MAX_TOKENS 512
#define MAX_COMMANDS 64

static volatile pid_t current_child = 0;

// Manejador de señal SIGINT (Ctrl-C)
void sigint_handler(int sig) {
    (void)sig;
    if (current_child > 0) {
        kill(current_child, SIGINT);
    }
}

// Elimina espacios/tabuladores/nuevas líneas al inicio y fin
char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n')) *end-- = '\0';
    return s;
}

// Divide una línea en comandos separados por '|'
int split_pipeline(char *line, char *commands[]) {
    int n = 0;
    char *saveptr;
    char *tok = strtok_r(line, "|", &saveptr);
    while (tok && n < MAX_COMMANDS) {
        commands[n++] = trim(tok);
        tok = strtok_r(NULL, "|", &saveptr);
    }
    return n;
}

// Parsea un comando en un arreglo argv (modifica la cadena de entrada)
char **parse_args(char *cmd) {
    char **argv = malloc(sizeof(char*) * (MAX_TOKENS+1));
    int i = 0;
    char *saveptr;
    char *tok = strtok_r(cmd, " \t\n", &saveptr);
    while (tok && i < MAX_TOKENS) {
        argv[i++] = tok;
        tok = strtok_r(NULL, " \t\n", &saveptr);
    }
    argv[i] = NULL;
    return argv;
}

// Ejecuta una tubería de comandos (arreglo commands con n elementos)
int execute_pipeline(char *commands[], int n) {
    int i;
    int in_fd = STDIN_FILENO;
    pid_t pids[MAX_COMMANDS];

    for (i = 0; i < n; ++i) {
        int pipefd[2] = {-1, -1};
        if (i < n-1) {
            if (pipe(pipefd) == -1) {
                perror("pipe");
                return -1;
            }
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return -1;
        }
        if (pid == 0) {
            if (in_fd != STDIN_FILENO) {
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }
            if (i < n-1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            // Argumentos
            char *cmdcopy = strdup(commands[i]);
            char **argv = parse_args(cmdcopy);
            if (argv[0] == NULL) exit(0);
            execvp(argv[0], argv);
            // Si execvp retorna, hubo error
            fprintf(stderr, "mishell: %s: %s\n", argv[0], strerror(errno));
            exit(127);
        } else {
            pids[i] = pid;
            if (in_fd != STDIN_FILENO) close(in_fd);
            if (i < n-1) {
                close(pipefd[1]);
                in_fd = pipefd[0];
            }
        }
    }

    // Esperar la ejecución en primer plano
    int status = 0;
    for (i = 0; i < n; ++i) {
        current_child = pids[i];
        waitpid(pids[i], &status, 0);
    }
    current_child = 0;
    return status;
}

// Ejecuta un comando único y opcionalmente mide tiempo y recursos
int run_and_profile(char **argv, int save_to_file, const char *filename, int timeout_seconds) {
    struct timespec start, end;
    struct rusage usage;
    pid_t pid;

    int tmpfd = -1;
    char tmpname[] = "/tmp/miprof_out_XXXXXX";

    if (save_to_file) {
        tmpfd = mkstemp(tmpname);
        if (tmpfd == -1) {
            perror("mkstemp");
            return -1;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    pid = fork();
    if (pid == -1) { perror("fork"); return -1; }
    if (pid == 0) {
        if (save_to_file) {
            dup2(tmpfd, STDOUT_FILENO);
            dup2(tmpfd, STDERR_FILENO);
            close(tmpfd);
        }
        if (timeout_seconds > 0) {
            // activar alarma local
            alarm(timeout_seconds);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "mishell: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    current_child = pid;

    int status = 0;
    if (timeout_seconds > 0) {
        // Espera con límite de tiempo
        int waited = 0;
        while (1) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) break;
            if (w == -1) { perror("waitpid"); break; }
            if (waited >= timeout_seconds) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            sleep(1);
            waited++;
        }
    } else {
        waitpid(pid, &status, 0);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    getrusage(RUSAGE_CHILDREN, &usage);

    double real_sec = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/1e9;
    double usr_sec = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec/1e6;
    double sys_sec = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec/1e6;
    long maxrss = usage.ru_maxrss; // en Linux: kilobytes

    // Crear resumen
    char summary[1024];
    int n = snprintf(summary, sizeof(summary),
        "Comando: %s\nReal: %.6fs  Usuario: %.6fs  Sistema: %.6fs  MaxRSS: %ld\nExitStatus: %d\n",
        argv[0], real_sec, usr_sec, sys_sec, maxrss, WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    if (save_to_file && tmpfd != -1) {
        // Guardar salida + resumen en archivo
        int outfd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (outfd == -1) {
            perror("abrir archivo de salida");
        } else {
            dprintf(outfd, "---- miprof append: %s ----\n", argv[0]);
            lseek(tmpfd, 0, SEEK_SET);
            char buf[4096];
            ssize_t r;
            while ((r = read(tmpfd, buf, sizeof(buf))) > 0) write(outfd, buf, r);
            write(outfd, summary, n);
            write(outfd, "\n", 1);
            close(outfd);
        }
        close(tmpfd);
        unlink(tmpname);
    } else if (save_to_file == 0) {
        // Mostrar resumen en pantalla
        write(STDOUT_FILENO, summary, n);
    }

    current_child = 0;
    return status;
}

// Procesa un comando
int handle_single_command(char *cmdline) {
    char *copy = strdup(cmdline);
    char **argv = parse_args(copy);
    if (argv[0] == NULL) { free(argv); free(copy); return 0; }

    if (strcmp(argv[0], "exit") == 0) {
        exit(0);
    }
    if (strcmp(argv[0], "cd") == 0) {
        if (argv[1]) chdir(argv[1]);
        free(argv); free(copy);
        return 0;
    }

    if (strcmp(argv[0], "miprof") == 0) {
        if (!argv[1]) {
            fprintf(stderr, "uso: miprof [ejec|ejcsave archivo|maxtiempo segs] comando args...\n");
            free(argv); free(copy); return 0;
        }
        if (strcmp(argv[1], "ejec") == 0) {
            if (!argv[2]) { fprintf(stderr, "no se indicó comando para ejec\n"); }
            else {
                char **cmd = &argv[2];
                run_and_profile(cmd, 0, NULL, 0);
            }
        } else if (strcmp(argv[1], "ejecsave") == 0) {
            if (!argv[2] || !argv[3]) { fprintf(stderr, "uso: miprof ejecsave archivo comando args...\n"); }
            else {
                const char *file = argv[2];
                char **cmd = &argv[3];
                run_and_profile(cmd, 1, file, 0);
            }
        } else if (strcmp(argv[1], "maxtiempo") == 0) {
            if (!argv[2] || !argv[3]) { fprintf(stderr, "uso: miprof maxtiempo segs comando args...\n"); }
            else {
                int secs = atoi(argv[2]);
                char **cmd = &argv[3];
                run_and_profile(cmd, 0, NULL, secs);
            }
        } else {
            fprintf(stderr, "miprof: modo desconocido %s\n", argv[1]);
        }
        free(argv); free(copy);
        return 0;
    }

    // Si no ejecutar como comando externo
    char *single = strdup(cmdline);
    char *commands[2]; commands[0] = single; commands[1] = NULL;
    int status = execute_pipeline(commands, 1);
    free(single);
    free(argv); free(copy);
    return status;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    char *line = NULL;
    size_t len = 0;

    while (1) {
        // Prompt
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) printf("mishell:%s$ ", cwd);
        else printf("mishell$ ");
        fflush(stdout);

        ssize_t nread = getline(&line, &len, stdin);
        if (nread == -1) {
            // EOF (Ctrl-D)
            printf("\n");
            break;
        }
        // Línea vacía
        if (nread == 1) continue;

        // Quitar '\n' final
        if (line[nread-1] == '\n') line[nread-1] = '\0';
        char *trimmed = trim(line);
        if (strlen(trimmed) == 0) continue;

        // Dividir por tuberías
        char *linecopy = strdup(trimmed);
        char *commands[MAX_COMMANDS];
        int ncmds = split_pipeline(linecopy, commands);

        if (ncmds <= 1) {
            handle_single_command(trimmed);
        } else {
            // Ejecutar tubería (cada comando debe estar en buffer separado)
            char *cmdcopies[MAX_COMMANDS];
            for (int i = 0; i < ncmds; ++i) {
                cmdcopies[i] = strdup(commands[i]);
            }
            execute_pipeline(cmdcopies, ncmds);
            for (int i = 0; i < ncmds; ++i) free(cmdcopies[i]);
        }

        free(linecopy);
    }

    free(line);
    return 0;
}
