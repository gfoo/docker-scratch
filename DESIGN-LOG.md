# Design Log — Conteneurs, kernel partagé & image `scratch`

> Notes issues d'une discussion exploratoire. Point de départ : « pourquoi MongoDB 8
> segfault sur kernel 7 alors que Docker isole le conteneur ? » → on remonte jusqu'à
> construire le plus petit conteneur possible à la main.

---

## 1. Le malentendu fondateur : Docker ≠ VM

Un conteneur **n'a pas son propre kernel**. C'est juste des process de l'hôte cloisonnés par :
- **namespaces** → isolation de la vue (process, réseau, montages, users)
- **cgroups** → limites de ressources (CPU, RAM)

```
┌─────────────── VM ───────────────┐   ┌──────────── Docker ────────────┐
│  app                             │   │  app (conteneur)               │
│  libc                            │   │  libc          ← dans l'image  │
│  kernel invité (le sien)         │   │  (PAS de kernel ici)           │
│  hyperviseur                     │   │  ───────────────────────────   │
│  kernel hôte                     │   │  kernel HÔTE (partagé)         │
└──────────────────────────────────┘   └─────────────────────────────────┘
```

**Isolé par Docker :** le userland (FS, binaires, libs, **y compris la libc**).
**PAS isolé :** le kernel — partagé entre l'hôte et tous les conteneurs.

---

## 2. Conséquence : le bug Mongo 8 / kernel 7

Chaîne d'appel : `mongod` → sa libc (dans l'image) → **syscall** → **kernel HÔTE**.

- La libc est figée dans l'image → ce n'est **pas** elle le problème.
- La vraie frontière, c'est l'**ABI des syscalls** (userland ↔ kernel).
- Kernel 7.0 change un comportement bas niveau dont mongod 8.x dépend (alloc mémoire /
  mmap / tcmalloc) → segfault. Ce n'est PAS une histoire de libc, ni une « spec libc cassée ».

**Workaround retenu :** booter un kernel plus ancien (`6.11-intel`) pour le mongod local.
On change la seule pièce que le conteneur ne peut pas isoler : le kernel partagé.

> Règle d'or Linux « we don't break userspace » → l'ABI syscall est censée être stable,
> mais des régressions arrivent, et MongoDB tape dans les coins sensibles.

---

## 3. `FROM alpine` n'apporte PAS de kernel

- Image Docker `alpine` ≈ 5 Mo = **userland seul** (busybox + musl + apk). Pas de `/boot`,
  pas de `vmlinuz`.
- À ne pas confondre avec **la distribution Alpine** (l'ISO) qui, elle, embarque un kernel.
- Image de conteneur = **demi-distribution** (userland sans kernel).

Preuve vérifiée en direct :
```
docker run --rm alpine uname -r   → 7.0.0-22-generic
uname -r            (hôte)        → 7.0.0-22-generic   # MÊME kernel
```

---

## 4. « On dépend toujours de l'hôte ? À quoi ça sert alors ? »

Sur Linux natif : **oui, toujours.** Le but des conteneurs n'a jamais été l'isolation kernel.
Ce qu'ils résolvent :

| Apport            | Concrètement                                            |
|-------------------|---------------------------------------------------------|
| Reproductibilité  | image fige libc/libs/versions → même userland partout   |
| Packaging         | un seul artefact = app + toutes ses deps                |
| Densité / vitesse | pas de kernel à booter → démarrage en ms, forte densité |
| Limites resources | cgroups (RAM/CPU par conteneur)                         |
| Cloisonnement     | namespaces — sécurité *raisonnable*, pas *forte*        |

Compromis : on sacrifie l'isolation kernel (qu'offre une VM) pour la légèreté.
Le bug Mongo/kernel est la facture de ce compromis.

### Podman ne règle PAS ce problème
Mêmes conteneurs OCI, **même kernel hôte partagé**. Ses plus : daemonless + rootless
(meilleure posture sécurité), mais **isolation kernel identique à Docker**.

### Les vrais systèmes « mieux isolés » (ils REMETTENT un kernel)
| Techno         | Mécanisme                                  | Isolation kernel |
|----------------|--------------------------------------------|------------------|
| Kata Containers| micro-VM par conteneur                     | ✅ vraie         |
| Firecracker    | microVM minimaliste (AWS Lambda/Fargate)   | ✅ vraie         |
| gVisor         | kernel réimplémenté en userspace (Go)      | ✅ forte         |
| VM classique   | kernel invité complet + hyperviseur        | ✅ maximale      |

---

## 5. Docker sous Windows/Mac : une VM Linux cachée

Une image Alpine (userland Linux) **tourne** sous Docker Desktop Windows — mais via une
**VM Linux cachée** (WSL2 aujourd'hui, Hyper-V avant) qui fournit le kernel Linux.

```
Windows (kernel NT)
 └─ WSL2 / Hyper-V
     └─ VM Linux (kernel LINUX ← le vrai kernel des conteneurs)
         └─ conteneur alpine
```

Implication : sous Windows le kernel est celui de WSL2 (souvent 5.15 / 6.6 LTS), **pas** le
7.0 → le même conteneur Mongo pourrait tourner sans crash. D'où « ça marche chez lui pas
chez moi » : kernels différents en dessous.

Les 2 seuls cas où Alpine ne tourne pas :
1. mode « Windows containers » (partage le kernel NT) — rare, explicite.
2. mauvaise archi CPU (arm64 vs amd64) sans émulation.

---

## 6. La base de la base : `scratch` + busybox statique

`scratch` = image **vide** officielle (0 octet, aucun fichier). On ne peut pas partir de
plus bas. Pour avoir un `sh`, il faut y mettre **un** binaire **statique**.

```dockerfile
FROM scratch
ADD busybox /bin/busybox
RUN ["/bin/busybox", "--install", "/bin"]   # crée sh, ls, cat... → liens vers busybox
ENTRYPOINT ["/bin/busybox", "sh"]
```

Récupérer un busybox statique tout prêt :
```bash
docker create --name tmp busybox:musl
docker cp tmp:/bin/busybox ./busybox
docker rm tmp

docker build -t minimal .
docker run --rm -it minimal        # → on tombe dans un sh
```

Pièges :
- Le binaire **doit être statique** (sinon il cherche `/lib/libc.so` absent → `no such file`).
- `scratch` enlève du *userland*, **jamais** le kernel (toujours celui de l'hôte/VM).
- busybox statique ≈ 1–2 Mo, ~300 commandes Unix.

---

## 7. Le « busybox » minimal en C : un shell en ~15 lignes

Essence d'un shell = **lire une ligne → `fork` → `exec` → `wait`**.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    char line[256];
    while (1) {
        write(1, "$ ", 2);                              // prompt
        if (!fgets(line, sizeof line, stdin)) break;    // EOF (Ctrl-D) → on sort
        line[strcspn(line, "\n")] = 0;                  // vire le \n

        char *argv[16]; int i = 0;                      // découpe en mots
        for (char *t = strtok(line, " "); t && i < 15; t = strtok(NULL, " "))
            argv[i++] = t;
        argv[i] = NULL;
        if (i == 0) continue;

        pid_t pid = fork();                             // fork
        if (pid == 0) {                                 // enfant
            execvp(argv[0], argv);                      // exec → remplace l'image process
            perror(argv[0]); _exit(127);                // execvp revenu = échec
        }
        waitpid(pid, NULL, 0);                          // parent attend
    }
    return 0;
}
```

Compilation statique pour `scratch` :
```bash
gcc -static -Os -o sh tinysh.c && strip sh   # glibc statique ≈ 700 Ko–1 Mo
musl-gcc -static -Os -o sh tinysh.c          # musl ≈ 20–30 Ko
```

Points clés :
- `fork` duplique le process, `execvp` remplace le binaire de l'enfant, `waitpid` attend.
  Bash = pareil + 100k lignes (pipes, jobs, variables, globbing…).
- `execvp` ne **retourne jamais** en cas de succès (il écrase l'image mémoire). Si la ligne
  d'après s'exécute → l'exec a échoué.
- Ce binaire ne parle qu'en **syscalls** (`fork`, `execve`, `wait4`, `read`, `write`) :
  exactement la frontière userland↔kernel. Il parle direct au kernel de l'hôte.

Encore plus petit : pas de libc, `_start` au lieu de `main`, syscalls en asm inline →
quelques centaines d'octets. Mais ce n'est plus du C portable.

---

## TODO / à essayer dans cette session
- [ ] Compiler `tinysh.c` en statique (glibc puis musl, comparer la taille).
- [ ] Builder une image `FROM scratch` avec ce binaire comme ENTRYPOINT.
- [ ] `docker run --rm -it` → vérifier qu'on a un prompt et qu'`exec` marche.
- [ ] Comparer la taille image : scratch+tinysh vs scratch+busybox vs alpine.
- [ ] (bonus) variante sans libc avec syscalls en asm — voir jusqu'où on descend.
