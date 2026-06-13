# docker-scratch

Exploration : conteneurs, kernel partagé et image `scratch` — jusqu'au conteneur
de 23 ko contenant un shell écrit à la main.

- **[DESIGN-LOG.md](DESIGN-LOG.md)** — le *pourquoi* : notes de la discussion
  exploratoire (Docker ≠ VM, bug Mongo 8 / kernel 7, `scratch`, tinysh).
- **[TUTORIAL.md](TUTORIAL.md)** — le *comment* : tutoriel pas à pas reproductible,
  avec les sorties mesurées. Passe par la case `chroot` (l'ancêtre de Docker) pour
  montrer que Docker = `chroot` + namespaces + cgroups.

## Fichiers

| Fichier | Rôle |
|---------|------|
| `tinysh.c` | shell minimal (~25 lignes) : `read` → `fork` → `exec` → `wait` |
| `Dockerfile.busybox` | image `scratch` + busybox statique (~300 commandes) |
| `Dockerfile.tinysh` | image `scratch` + tinysh → **22,7 ko** |

## Démarrage rapide

```bash
# voir TUTORIAL.md pour le détail de chaque étape
docker create --name tmp busybox:musl && docker cp tmp:/bin/busybox ./busybox && docker rm tmp
docker build -f Dockerfile.busybox -t scratch-busybox .

docker run --rm -v "$PWD":/w -w /w alpine sh -c \
  'apk add -q gcc musl-dev && gcc -static -Os -o sh tinysh.c && strip sh'
docker build -f Dockerfile.tinysh -t scratch-tinysh .
docker run --rm -it scratch-tinysh
```
