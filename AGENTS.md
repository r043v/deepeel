# Working on memscan

memscan découvre la memory map des ROMs : registre matériel par plateforme,
exploration autonome à fourches (les deux issues de chaque conditionnel, forks
d'IRQ), tout consigné (exécution, lectures de données, accès registres, copies
ROM→VRAM, changements de banque), stats de nécessité par section, fenêtre SDL2
temps réel. Démarrer par `README.md`, puis `docs/GB.fr.md`.

## Règles

- **Aucune ROM dans le dépôt.** Les fixtures de test sont générées à la
  construction par `tests/build_fixtures.py` dans le répertoire de build ;
  les ROMs réelles restent hors du dépôt (essais manuels dans `output/`,
  ignoré).
- Les changements du moteur d'exploration doivent garder les 4 tests verts :
  `ctest --test-dir build --output-on-failure`. `test_explorer` épingle les
  comportements contractuels : les deux côtés d'un conditionnel, l'éviction
  par contexte, les enregistrements de copie (boucle CPU, OAM DMA), la
  timeline de banques, le verdict `required`/`forced`, le bornage des boucles.
- Un fork d'exploration est toujours borné : si un nouveau chemin peut
  boucler, il lui faut un garde (budget de pas, cap d'arrière-branche, cap de
  forks par site) et son compteur de saturation rapporté. Rien ne doit
  tourner sans fin, et une saturation ne doit jamais être silencieuse.
- Les clés ROM sont des index compacts `bank * 0x4000 + (addr & 0x3FFF)`
  (`ms_rom_index`) — jamais `bank << 16`. Toute nouvelle structure indexée par
  la ROM utilise cet index.
- L'exécution hors ROM (pc ≥ $8000, code en VRAM/WRAM) est comptée
  (`exec_outside`) mais ne pénètre pas les cartes de la ROM.
- Un changement visible dans les rapports ou la fenêtre incrémente la version
  dans `CMakeLists.txt` (projet) ou est noté dans `docs/GB.fr.md`.
- Les nombres produits sur ROMs réelles sont des observations de l'outil
  (machine concrète, RAM à zéro, registres événementiels statiques) — pas des
  mesures de matériel. Ne pas les présenter comme telles.
- Langue : code et identifiants en anglais, commentaires en anglais, docs
  utilisateur en français.

## Commits

- L'auteur d'un commit reste la session git (r043v) ; un agent qui a fait le
  travail signe avec `Co-Authored-By: GLM <noreply@z.ai>` (ou son propre nom
  pour un autre agent). Jamais de travail d'agent anonyme.
