/* tinysh — l'essence d'un shell : lire une ligne → fork → exec → wait.
 * Voir DESIGN-LOG.md §7 et TUTORIAL.md étape 4. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    char line[256];
    while (1) {
        write(1, "$ ", 2);                              /* prompt */
        if (!fgets(line, sizeof line, stdin)) break;    /* EOF (Ctrl-D) → on sort */
        line[strcspn(line, "\n")] = 0;                  /* vire le \n */

        char *argv[16]; int i = 0;                      /* découpe en mots */
        for (char *t = strtok(line, " "); t && i < 15; t = strtok(NULL, " "))
            argv[i++] = t;
        argv[i] = NULL;
        if (i == 0) continue;

        pid_t pid = fork();                             /* fork */
        if (pid == 0) {                                 /* enfant */
            execvp(argv[0], argv);                      /* exec → remplace l'image process */
            perror(argv[0]); _exit(127);                /* execvp revenu = échec */
        }
        waitpid(pid, NULL, 0);                          /* parent attend */
    }
    return 0;
}
