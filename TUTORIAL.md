# Tutoriel pas à pas — du « Docker ≠ VM » au conteneur de 23 ko

> Compagnon pratique de [DESIGN-LOG.md](DESIGN-LOG.md). Le design log raconte le
> *pourquoi* (la discussion) ; ce tutoriel donne le *comment* (les manipulations),
> étape par étape, avec les sorties réellement obtenues sur la machine de référence
> (kernel `7.0.0-22-generic`, Docker 28.x, x86-64).
>
> **Fil rouge :** un conteneur n'a pas de kernel. On va le prouver, comprendre ce que
> ça implique (le bug MongoDB 8 / kernel 7), puis construire le plus petit conteneur
> possible : une image de **22,7 ko** contenant un shell écrit à la main en ~25 lignes de C.

---

## Prérequis

- Linux x86-64 avec Docker (ou Podman, mêmes commandes)
- `gcc` pour l'étape 4 (le variant musl se fait *dans un conteneur*, rien à installer)
- Les fichiers de ce repo : `tinysh.c`, `Dockerfile.busybox`, `Dockerfile.tinysh`

```bash
git clone https://github.com/gfoo/docker-scratch.git
cd docker-scratch
```

---

## Étape 1 — Prouver que le conteneur partage le kernel de l'hôte

*(correspond au DESIGN-LOG §1 et §3)*

Avant de rien construire, vérifions l'affirmation centrale : **un conteneur n'a pas
son propre kernel**. C'est juste un groupe de process de l'hôte, cloisonnés par des
*namespaces* (isolation de la vue : PID, réseau, montages…) et des *cgroups*
(limites CPU/RAM).

```bash
uname -r                          # kernel de l'hôte
docker run --rm alpine uname -r   # kernel "vu" depuis un conteneur Alpine
```

Résultat obtenu :

```
--- hôte ---
7.0.0-22-generic
--- conteneur alpine ---
7.0.0-22-generic
```

**Même version, au caractère près.** L'image `alpine` (~8 Mo) ne contient que du
*userland* : busybox, la libc musl, le gestionnaire de paquets `apk`. Pas de `/boot`,
pas de `vmlinuz`. Quand `uname` s'exécute dans le conteneur, il fait un *syscall* qui
atterrit directement dans le kernel de l'hôte.

> 💡 **À retenir :** une image de conteneur est une « demi-distribution » — le userland
> d'une distro, sans son kernel. Ne pas confondre l'*image Docker* `alpine` (5–8 Mo,
> sans kernel) avec l'*ISO* de la distribution Alpine (qui, elle, embarque un kernel).

### Pourquoi c'est important : le bug MongoDB 8 / kernel 7

C'est le point de départ du design log (§2). La chaîne d'appel d'un process
conteneurisé est :

```
mongod → sa libc (figée DANS l'image) → syscall → kernel de l'HÔTE
```

L'image fige tout le userland, libc comprise — donc la libc n'est jamais le problème.
La vraie frontière de compatibilité, c'est l'**ABI des syscalls** entre userland et
kernel. Si le kernel 7.0 change un comportement bas niveau (mmap, allocation mémoire)
dont mongod 8.x dépend, mongod segfault *dans son conteneur*, et aucune image ne peut
le protéger : le kernel est la seule pièce qu'un conteneur ne peut pas isoler.
(Workaround du log : booter un kernel 6.11 sur l'hôte.)

C'est aussi pour ça que « ça marche chez lui, pas chez moi » entre Linux natif et
Docker Desktop Windows/Mac : là-bas, les conteneurs tournent dans une **VM Linux
cachée** (WSL2/Hyper-V) avec un kernel souvent plus ancien (5.15/6.6 LTS) — voir §5
du design log.

---

## Étape 2 — `scratch` : l'image vide

*(DESIGN-LOG §6)*

`scratch` est l'image de base officielle **vide** : 0 octet, aucun fichier, aucun
shell, aucune libc. On ne peut pas partir de plus bas. Tout `FROM debian`, `FROM
alpine`… n'est qu'une pile de fichiers posée au-dessus de ce vide.

Conséquence immédiate : dans un Dockerfile `FROM scratch`, **la forme shell de `RUN`
ne marche pas** (`RUN echo hi` est traduit en `/bin/sh -c "echo hi"`, et il n'y a pas
de `/bin/sh`). Seule la forme *exec* `RUN ["binaire", "arg"]` fonctionne — et encore,
uniquement si le binaire est **statique** (un binaire dynamique chercherait
`/lib/ld-linux.so` et la libc, absents).

---

## Étape 3 — Premier conteneur habitable : `scratch` + busybox statique

Busybox regroupe ~300 commandes Unix (`sh`, `ls`, `cat`…) dans **un seul binaire**.
La variante `busybox:musl` est liée statiquement : parfaite pour `scratch`.

### 3.1 Récupérer le binaire sans rien installer

On ne télécharge rien à la main : on l'extrait d'une image Docker existante.

```bash
docker create --name tmp busybox:musl   # crée le conteneur SANS le démarrer
docker cp tmp:/bin/busybox ./busybox    # copie le binaire vers l'hôte
docker rm tmp
file busybox
```

Résultat :

```
busybox: ELF 64-bit LSB pie executable, x86-64, static-pie linked, stripped   (1.2M)
```

`static-pie linked` = aucune dépendance externe. C'est le critère décisif.

### 3.2 Construire l'image

`Dockerfile.busybox` (fourni dans le repo) :

```dockerfile
FROM scratch
ADD busybox /bin/busybox
RUN ["/bin/busybox", "--install", "/bin"]   # crée sh, ls, cat... → liens vers busybox
ENTRYPOINT ["/bin/busybox", "sh"]
```

```bash
docker build -f Dockerfile.busybox -t scratch-busybox .
docker run --rm -it scratch-busybox          # → un vrai shell interactif
```

Test rapide non interactif :

```bash
docker run --rm scratch-busybox -c 'echo hello from $(uname -r); ls /bin | head -5'
```

```
hello from 7.0.0-22-generic     ← toujours le kernel de l'hôte, évidemment
[
[[
acpid
add-shell
addgroup
```

> 💡 **Piège overlayfs constaté en pratique :** l'image fait 2,48 Mo, soit *plus* que
> `busybox:musl` (1,53 Mo) alors qu'on est parti de zéro ! Cause : `busybox --install`
> crée des **hard links** dans la couche du `RUN` ; or overlayfs ne peut pas
> hard-linker vers une couche inférieure → il fait un *copy-up* : le binaire de 1,2 Mo
> existe en double (couche `ADD` + couche `RUN`). Avec `--install -s` (symlinks),
> l'image retomberait à ~1,2 Mo. Bel exemple de « chaque instruction = une couche ».

---

## Étape 4 — Écrire son propre « busybox » : un shell en ~25 lignes de C

*(DESIGN-LOG §7)*

Busybox, c'est déjà 1,2 Mo et 300 commandes. Peut-on descendre plus bas ? Oui : en
écrivant l'essence d'un shell soi-même. Un shell, fondamentalement, c'est une boucle :

```
lire une ligne → fork() → exec() → wait()
```

Le code complet est dans [`tinysh.c`](tinysh.c). Les trois syscalls qui comptent :

| Appel | Rôle |
|-------|------|
| `fork()` | duplique le process courant (parent + enfant identiques) |
| `execvp()` | **remplace** l'image mémoire de l'enfant par le programme demandé |
| `waitpid()` | le parent se bloque jusqu'à la fin de l'enfant |

Deux subtilités à bien comprendre dans le code :

- `execvp` **ne retourne jamais en cas de succès** — il écrase tout le process. Si la
  ligne suivante (`perror`) s'exécute, c'est *forcément* que l'exec a échoué.
- Ce binaire ne parle au monde qu'en syscalls (`fork`, `execve`, `wait4`, `read`,
  `write`) : il matérialise exactement la frontière userland↔kernel de l'étape 1.
  Bash fait pareil, plus ~100 000 lignes pour les pipes, jobs, variables, globbing.

### 4.1 Compilation statique avec glibc

```bash
gcc -static -Os -o sh-glibc tinysh.c && strip sh-glibc
ls -lh sh-glibc
```

```
-rwxrwxr-x 775K sh-glibc
```

775 ko pour 25 lignes ! La glibc statique embarque énormément de machinerie (locales,
NSS, allocateur…). C'est le prix de `-static` avec glibc.

### 4.2 Compilation statique avec musl — dans un conteneur

Si `musl-gcc` n'est pas installé sur l'hôte, inutile de l'installer : on compile
*dans un conteneur Alpine* (où gcc est nativement lié à musl) — c'est l'esprit même
du sujet :

```bash
docker run --rm -v "$PWD":/w -w /w alpine sh -c \
  'apk add -q gcc musl-dev && gcc -static -Os -o sh-musl tinysh.c && strip sh-musl'
ls -lh sh-glibc sh-musl
```

```
-rwxrwxr-x 775K sh-glibc
-rwxr-xr-x  23K sh-musl     ← 33× plus petit
```

> 💡 musl est conçue pour la liaison statique : pas de NSS, pas de locales lourdes,
> code minimal. D'où les 23 ko contre 775 ko. Mêmes 25 lignes de C, même comportement.

---

## Étape 5 — L'image finale : `scratch` + tinysh = 22,7 ko

`Dockerfile.tinysh` (fourni) :

```dockerfile
FROM scratch
ADD sh /sh
ENTRYPOINT ["/sh"]
```

```bash
cp sh-musl sh
docker build -f Dockerfile.tinysh -t scratch-tinysh .
docker run --rm -it scratch-tinysh
```

On tombe sur le prompt `$ `. Attention : l'image ne contient **que** `/sh` — il n'y a
ni `ls` ni `echo` à exécuter ! Deux expériences instructives :

```
$ hello
hello: No such file or directory   ← execvp a échoué → perror (cf. étape 4)
$ /sh
$                                   ← un tinysh enfant lancé PAR tinysh : fork/exec marchent
```

(Ctrl-D pour sortir du shell enfant, puis du parent.)

Test non interactif équivalent :

```bash
printf 'hello\n/sh\n' | docker run --rm -i scratch-tinysh
```

```
$ hello: No such file or directory
$ $ $
```

---

## Étape 6 — Bilan des tailles

```bash
docker images | grep -E 'scratch-tinysh|scratch-busybox|alpine|busybox'
```

Résultats mesurés :

| Image | Taille | Contenu |
|-------|--------|---------|
| `scratch-tinysh` | **22,7 ko** | 1 binaire musl statique, 1 « commande » |
| `busybox:musl` | 1,53 Mo | busybox + métadonnées |
| `scratch-busybox` | 2,48 Mo | busybox ×2 (copy-up overlayfs, cf. étape 3) |
| `alpine` | 8,45 Mo | busybox + musl + apk + arborescence complète |

Du million d'octets d'Alpine aux 23 ko de tinysh, **rien ne change côté kernel** : les
quatre images utilisent exactement le même kernel hôte. On n'a fait que réduire le
userland embarqué.

---

## Bonus (non réalisé) — descendre sous le kilo-octet

Dernière marche du design log : supprimer la libc elle-même. `_start` au lieu de
`main`, syscalls écrits en assembleur inline (`syscall` x86-64 avec les numéros de
`/usr/include/asm/unistd_64.h`), link avec `-nostdlib -static`. On descend à quelques
centaines d'octets — mais ce n'est plus du C portable. Exercice laissé au lecteur ;
pistes : §7 fin du design log.

---

## Ce qu'il faut retenir

1. **Conteneur = process cloisonné, pas mini-VM.** Namespaces + cgroups isolent le
   userland ; le kernel reste celui de l'hôte, partagé par tous.
2. **La frontière de compatibilité est l'ABI syscall**, pas la libc (elle est figée
   dans l'image). D'où des bugs du type Mongo 8 / kernel 7, qu'aucune image ne peut
   éviter — et qui se règlent côté hôte (changer de kernel) ou en remettant un kernel
   par conteneur (Kata, Firecracker, gVisor — DESIGN-LOG §4).
3. **Une image n'est qu'un tas de fichiers.** `scratch` est le tas vide ; il suffit
   d'un binaire statique pour avoir un conteneur fonctionnel de 23 ko.
4. **Statique ou rien sur `scratch`** : pas de libc dans l'image → un binaire
   dynamique meurt avec `no such file or directory` avant même son `main`.
5. **Un shell n'a rien de magique** : `read` → `fork` → `exec` → `wait`. Tout le
   reste (bash, zsh…) est du confort au-dessus de ces quatre syscalls.
