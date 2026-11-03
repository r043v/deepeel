# Working on deepeel

deepeel découvre la memory map des ROMs : registre matériel par plateforme,
exploration autonome à fourches (les deux issues de chaque conditionnel, forks
d'IRQ), tout consigné (exécution, lectures de données, accès registres, copies
ROM→VRAM, changements de banque), stats de nécessité par section, fenêtre SDL2
temps réel. Démarrer par `README.md`, puis `docs/GB.fr.md`. La norme de
couverture commune à toutes les plateformes (techniques exigées, typage,
sources, frontière différée, feuille de route) est `docs/COUVERTURE.fr.md`.

## Règles

- **Aucune ROM dans le dépôt.** Les fixtures de test sont générées à la
  construction par `tests/build_fixtures.py` dans le répertoire de build ;
  les ROMs réelles restent hors du dépôt (essais manuels dans `output/`,
  ignoré).
- Les changements du moteur d'exploration doivent garder les 5 tests verts :
  `ctest --test-dir build --output-on-failure`. `test_explorer` épingle les
  comportements contractuels : les deux côtés d'un conditionnel, l'éviction
  par contexte, les enregistrements de copie (boucle CPU, OAM DMA), la
  timeline de banques, le verdict `required`/`forced`, le bornage des boucles.
- Un fork d'exploration est borné par défaut (budget de pas, cap d'arrière-branche,
  cap de forks par site, compteur de saturation rapporté, jamais silencieux). Exception
  décidée par le propriétaire : `--unbounded` lève ces plafonds (cap de forks, file,
  spool, chemins, pas, arrêt mémoire) ; ce que la RAM ne tient pas va sur le disque
  (`spool.bin` dans `<sortie>.state`) pour exploration ultérieure. La couverture prime
  sur le temps et les ressources ; le seul arrêt est l'épuisement ou SIGINT. Le
  pruning « les deux côtés déjà couverts » reste : il n'ajoute aucun code.
- Les clés ROM sont des index compacts `bank * 0x4000 + (addr & 0x3FFF)`
  (`dp_rom_index`) — jamais `bank << 16`. Toute nouvelle structure indexée par
  la ROM utilise cet index.
- L'exécution hors ROM (pc ≥ $8000, code en VRAM/WRAM) est comptée
  (`exec_outside`). Quand une boucle de copie verbatim ROM→WRAM a produit ces
  octets, l'exécution est rattachée à leurs octets source en ROM
  (`exec_wram_rom`) : le code copié puis exécuté en WRAM est du code de la ROM.
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
