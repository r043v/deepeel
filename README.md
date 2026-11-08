# deepeel

Outil générique de découverte de la **memory map** des ROMs, plateforme par
plateforme. Il contient le registre matériel complet de chaque console
(registres IO, fenêtres de mapper, IRQ), il exécute la ROM dans la **machine
m0boy** (l'interpréteur GB dont le run_hash est tenu par la suite de tests
m0boy, embarquée comme dépendance `M0BOY_DIR`) et consigne **tout** : chaque
octet exécuté, chaque lecture de donnée ROM, chaque accès registre, chaque
copie ROM→VRAM/OAM, chaque changement de banque.

La recherche est **autonome** : à chaque branchement conditionnel les deux
issues sont testées (fork), à chaque point d'interruption activée un fork
d'IRQ est tenté. L'éviction interdit les ré-exploitations inutiles :

- dédup exact : un état (PC + registres + banques + RAM hachée par pages)
  déjà exploré n'est jamais ré-engendré ;
- éviction « même contexte » : une ré-entrée dans une section dont l'état
  d'entrée ne diffère que par des composantes que la section ne lit jamais
  (masse de liveness apprise pendant l'exécution) est évincée ;
- anti-boucles : budget de pas par chemin, compteur par arrière-branche,
  budget global de pas, budget de forks par site — la saturation est
  rapportée, jamais cachée.

Toute l'exploration vit dans un **arbre commun** (`src/explore/tree.c`) :
chaque graine, chaque fourche de branche/IRQ, chaque dépôt de session est un
choix relié à son parent ; quand un chemin se termine son choix se ferme avec
une issue (`done`, `merged` — retombe sur du connu sans issue, le cas
gameover —, `dead` — opcode indéfini —, `reset` — retour à $0100, `budget`,
`capped`). Après les sessions, une **recherche totale DFS** draine la
frontière en profondeur (`tree.bin` la persiste, `--resume` la reprend).

Des **stats de nécessité par section** sont tenues pendant l'exploration
(hits, flux naturel vs forcé, contextes, côtés de conditionnels couverts,
registres touchés, copies produites) et chaque section reçoit un verdict :
`required` (atteint par le flux de boot, gère une IRQ, ou produit des
copies), `forced` (uniquement atteint par exploration forcée — candidat
donnée-avec-code) ou `speculative`.

La fenêtre **SDL2** montre la carte de la ROM en temps réel pendant
l'exploration (une tuile par banque, 16×16 blocs de 64 octets, zoom/pan,
clic = détail de section avec désassemblage).

## Construction

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

SDL2 est optionnel (`-DBUILD_GUI=OFF` pour s'en passer).

## Usage

```bash
deepeel explore rom.gb [--gui] [--json map.json] [--text report.txt] [--cov out.cov]
deepeel explore rom.gb --max-total 50000000 --queue 16384 --covered-keep 8 --seed-banks
deepeel explore rom.gb --entry 2:4000        # graine supplémentaire banque 2, $4000

# sessions jouées (m0boy : inputs aléatoires + attract, 10 threads, pool de
# checkpoints, scheduler d'époques, persistance --state-dir/--resume)
deepeel explore rom.gb --play --frames 72000 --state-dir s --cov jeu.cov --romdata jeu.prof
deepeel explore rom.gb --play --resume --state-dir s
# TAS : rejoue un movie tool-assisté (VBA .vmv/.vbm, BizHawk .bk2, log texte) —
# un worker par fichier ; le TAS est prioritaire sur tout budget (frames,
# segments, stagnation), le film est joué en entier jusqu'au bout du jeu,
# puis le worker passe en jeu aléatoire
deepeel explore rom.gb --play --tas supermarioland.bk2 --frames 18000
# découverte des films : tous les fichiers-movies d'un répertoire (8 max,
# meilleure ressemblance de titre d'abord), plus besoin de les nommer un à un
deepeel explore rom.gb --play --tas-dir output/tas --tas-solo
# murs de ressource : arrêt gracieux (pool sauvegardé, --resume reprend) après
# N secondes de jeu (films TAS compris) ou une fois le RSS au-dessus de MB MiB
deepeel explore rom.gb --play --time-limit 1800 --mem-cap 2000
# profil compact de la passe : résumé carte + liste complète des plages
# résiduelles + tous les compteurs (l'entrée de l'agrégation multi-ROMs)
deepeel explore rom.gb --play --summary run.summary.json
# fenêtre de surveillance seule (SDL2) : l'écran du film échantillonné à
# 1..30 images/s (défaut 30), sans ralentir le TAS ; Tab = film suivant
deepeel explore rom.gb --play --tas film.vbm --watch 10
# export jouable : ecrit le checkpoint choisi (le plus productif, ou
# --export-slot N) en slot 0 du <SD>.STA du projet pc m0boy — le firmware
# pc le propose au menu (Etat -> Charger l'etat) et boote dessus avec
# M0_STATE=0 ; le .SAV batterie suit quand la cartouche a de la RAM
deepeel explore rom.gb --play --frames 6000 --export-playable chemin/vers/projet_pc
# mode humain : une lane jouable au pad, chaque avancée sauvée en PNG (écran +
# machine complète, rechargeable --from-png) et explorée automatiquement
deepeel explore rom.gb --play --gui --human
deepeel map rom.gb                           # carte statique seule (pointeurs, padding)
deepeel regs gb                              # dump du registre matériel GB
deepeel regs msx                             # registre MSX1 ; pont machine en cours
deepeel dis rom.gb 2:4000 12                 # désassemblage banque 2, $4000
```

Options d'exploration : `--max-steps` (par chemin), `--max-paths`,
`--max-total` (mur global de pas), `--queue` + `--queue-max` (la file pleine
**double** jusqu'au plafond au lieu de jeter des chemins entiers ; la
croissance s'arrête au mur `--mem-cap` et chaque croissance est comptée),
`--loop-cap`, `--seed-cap`, `--seed-banks`, `--closure-rounds` (défaut 12),
`--covered-keep`, `--no-pointer-scan`, `--no-closure`, `--no-provenance`, `--no-siblings`, `--no-context-evict`, `--no-irq-fork`, `--no-key-fork`, `--no-p1-fork` (sondes manette : presses par IRQ joypad et lectures directes de `$FF00`).

La **provenance statique** (portage de la branche z80) prouve sans machine
les cibles des `jp hl` (chargements immédiats, tables parcourues pointeur en
main) et les octets de données qu'elles consultent — nouvelle classe
`donnée statique` sur la carte, compteurs `prov_*` dans le rapport.

Avec un `--state-dir`, le **spool disque** rend l'exploration arrêtable et
repreneuse à volonté : les états débordant de la file RAM attendent dans
`spool.bin` et un `--resume` relit tout (file + dedup `exact.bin`), même après
un kill. `deepeel maprender rom.gb in.map out.map.png` re-rend l'image de
couverture depuis un `.map` sauvegardé, sans re-scan.

L'export `--cov` écrit le format de couverture de m0boy (`bank addr flags
count`) — pont optionnel vers le convertisseur. `--map x` écrit par défaut la
carte PNG (1 pixel par octet + chaleur d'usage joué, chunks zTXt : carte,
`.cov` joué, profil ROM, usage par zone) que `gbrecomp --coverage x.map.png`
lit directement ; `--map x.map` écrit le texte.

### Campagne multi-ROMs

`tools/scan_all.py` applique la recette générique à tout un répertoire de
ROMs, puis agrège les profils :

```bash
# scan borné : 30 min de jeu par ROM (mur horloce gracieux), films TAS minés
# localement par ressemblance de titre (assignation exclusive, les suites ne
# polluent pas l'original), RAM plafonnée ; sorties dans
# output/batch/<titre>/{<titre>.summary.json, .txt, .map.png, .log, state/}
python3 tools/scan_all.py scan
# une ROM à fond (pas de mur horloge), en reprenant son pool sauvegardé
python3 tools/scan_all.py scan --only zelda --full --resume
# agrégation : couverture par ROM, anatomie du résidu, murs et saturations
python3 tools/scan_all.py digest            # -> output/batch/REPORT.md

# z80 : même principe pour SG-1000 / Master System / Game Gear / MSX1 —
# 20 M pas par phase, --seed-banks, --jobs 2, films TAS appariés par nom,
# résumé JSON par ROM ; agrégation en rapport de campagne
python3 tools/scan_all.py scan-z80 --system sms --roms ~/roms/sms
python3 tools/scan_all.py digest-z80 --out output/batch-z80/sms
```

## État

La norme visée pour toutes les plateformes (techniques de couverture
exigées, typage et sources par octet, frontière différée, mutualisation,
libretro, PNG adaptatif) et sa feuille de route sont décrites dans
[docs/COUVERTURE.fr.md](docs/COUVERTURE.fr.md). Le bilan des techniques
essayées, gardées ou abandonnées avec leurs mesures, est tenu dans
[docs/CATALOGUE.fr.md](docs/CATALOGUE.fr.md) ; le tableau comparatif des
campagnes par plateforme est [docs/COMPARATIF.fr.md](docs/COMPARATIF.fr.md)
(régénéré par `tools/compare_platforms.py`) ; l'interface de consommation
par m0boy (`.cov`, PNG, `.map`, code en RAM) est documentée dans
[docs/m0boy-consommation.fr.md](docs/m0boy-consommation.fr.md).

- **Game Boy** : la machine est celle de m0boy (MBC0/1/2/3/5, PPU/APU/timer
  réels, CPU interprété) — 17 variantes MBC×RAM compilées au build, un
  thread = une instance préfixée. Voir `docs/GB.fr.md` pour le registre
  matériel, l'arbre et les limites actuelles.
- **Z80 / SG-1000 / Master System / Game Gear** : première intégration des
  machines m0boy, exploration bornée à fourches, registres matériels,
  désassemblage et cartes texte via `deepeel z80 explore sms jeu.sms`.
  Les profils SMS/GG acceptent aussi `--mapper sega|codemasters|korean150`
  pour remplacer l’autodétection. Les graines GB sont portées : scan de
  tables de pointeurs par vues de banques, `--seed-banks`, `--entry`
  (0.14.0) ; sections exécutées avec éviction par contexte et verdicts de
  nécessité (0.15.0) ; file croissante, spool disque, `--resume` et arbre
  de choix (0.16.0) ; sessions jouées et verdict `played` (0.19.0) ; rejeu
  TAS prioritaire `--tas` (.bk2/log texte, 0.20.0) ; égouttage parallèle
  `--jobs 2` sur instances runtime clonées par `objcopy --redefine-syms`
  (0.21.0). Voir `docs/Z80.fr.md` pour les limites.
- **MSX1** : registre matériel et pont machine isolé (slots, mappers,
  PPI, AY, BIOS simulé), exploration dynamique bornée et cartes texte via
  `deepeel z80 explore msx jeu.rom --mapper auto`, puis fermeture statique
  sur les vues de slots et de banques observées. Voir
  [docs/MSX.fr.md](docs/MSX.fr.md).
- **Mega Drive** (0.13.0, jalon 2) : exploration à fourches sur le 68000 de
  m0boy (`MdFull`) — registre, désassembleur, deux issues des conditionnels,
  fork V-int, copies DMA ROM→VRAM. Pas encore de GUI. Voir
  `docs/MD.fr.md`.
- **NES** (0.28.0) : le runtime NES de m0boy embarqué (NROM, UxROM),
  exploration à fourches avec snapshots exacts pris au milieu d'une frame,
  rejeu TAS `.fm2`/`.bk2` et sessions jouées. Voir `docs/NES.fr.md`.
- Autres plateformes : l'architecture est prête (`src/platform/platform.h`,
  `src/cpu/`) — une plateforme = une table de registres + des hooks ; un CPU
  = un décodeur/exécuteur qui journalise ses effets. Z80, 6502, HuC6280,
  65816 sont les suivants naturels.

## Layout

- `src/core/` — cartouche, inflate/zip (les TAS `.bk2`), conteneur PNG.
- `src/cpu/sm83/` — décodeur SM83 statique (longueurs, conditions,
  désassemblage) ; l'exécution est celle de m0boy.
- `src/mach/` — le pont vers la machine m0boy embarquée (instances
  préfixées, hooks trace/lecture/écriture/vblank, images d'état, longjmp).
- `src/platform/gb/` — registre matériel GB (registres, IRQ, régions,
  entrées).
- `src/explore/` — moteur à fourches, dédup à deux niveaux, sections et
  stats de nécessité, suivi de copies, sessions jouées, fichiers TAS, arbre
  commun.
- `src/report/` — texte, `map.json`, export `.cov`.
- `src/gui/` — visionneuse SDL2 temps réel (+ police 5×7 embarquée).
- `tests/` — fixtures GB synthétiques générées à la construction (aucune ROM
  dans le dépôt) + tests C via ctest.
