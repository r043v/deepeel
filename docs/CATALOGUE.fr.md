# Catalogue des techniques essayées

Catalogue des techniques appliquées à la découverte de memory map, gardées ou
abandonnées, avec les mesures qui ont décidé. Une technique abandonnée reste
ici avec ses chiffres : ne pas la réessayer sans nouvel élément. Les nombres
sont des **observations de l'outil** (machine concrète, RAM à zéro), jamais
des mesures de matériel. La norme des techniques est `docs/COUVERTURE.fr.md`
(matrice §2.1) ; ce catalogue la complète en gardant la trace des essais
infructueux.

## Gardées (défaut)

| Technique | Plateformes | Mesure décisive | Référence |
|---|---|---|---|
| Fourches des deux côtés des conditionnels, dédup exact des états empilés | toutes | — | `docs/GB.fr.md`, tests `explorer` |
| Frontière différée sur disque (§6), jamais d'abandon silencieux | toutes | `--spool` reprend un lot interrompu ; Zelda 0.249.79 : lot repris après redémarrage | `docs/COUVERTURE.fr.md` §6 |
| Sections par course avec verdict required/forced (C1) | GB, NES, PCE, MD | — | matrice C1 |
| Dédup d'états exacts persistée (`exact.bin`) | GB | — | `docs/GB.fr.md` |
| `--exact-dedup` MD (C3 partielle, hachage ~200 Kio par fourche) | MD | `fix_md_pad_effect` : 95 fourches de moins, couverture identique ; désactivée par défaut à cause du coût | `docs/GB.fr.md` (0.125.0) |
| Bornage des ensembles internes (bank_seen, journal de découverte) avec compteurs jamais silencieux | GB | audits RAM ; caps `DP_BANK_SEEN_CAP_MAX` (4 M), `DP_DISC_CAP_MAX` (64 K) | `docs/GB.fr.md` (0.249.80) |
| Tables de pointeurs inter-banques (S1 NES) | NES | DuckTales + table croisée ; fixture `nes_table_xbank` | `docs/NES.fr.md` (0.249.81) |
| Conteneur `.bk2` partagé (noyau §7.4, premier module) | NES, Sega, PCE, MD | 1038 → 692 lignes de chargeurs, noyau 55 lignes ; tests verts ; MD : ligne SHA1 du header ne masque plus un faux MD5 | `docs/COUVERTURE.fr.md` §7.4 (0.249.87) |
| Bouffées aléatoires depuis les instantanés TAS (J3) | SG/SMS/GG/MSX | modèle GB porté ; fixture : 2 instantanés × 2 bouffées = 4 sessions, 2000 pas comptés ; test `z80_tas_rng` | `docs/Z80.fr.md` (0.249.86) |
| Politique de frontière §6 commune (`dp_frontier_room`, raisons `DP_R_*`) | les 5 explorateurs | sega : compteur inchangés, raison suivie dans le spool ; NES : opt-in mesuré **neutre** (DuckTales 16,96 % avec et sans) ; PCE : opt-in (fixture : reportées/reprises) ; MD : gain mesuré (fixture `--path-steps 50` : 57 courses contre 29, 28 reportées toutes reprises ; Sonic inchangé : 410 513 pas / 10 794 octets) | `docs/GB.fr.md` 0.249.89, `docs/NES.fr.md`, `docs/PCE.fr.md`, `docs/MD.fr.md` |
| `--summary` JSON de campagne | PCE | même forme que MD ; Raiden 5 M pas : 18,3 % connus ; ligne PCE du tableau comparatif | `docs/PCE.fr.md` (0.249.85) |

## Opt-in (mesurées, pas par défaut)

| Technique | Plateformes | Mesure | Référence |
|---|---|---|---|
| Éviction par contexte grossière (`--ctx-cap`) : pc + registres + SR | NES, PCE, MD | quasi no-op sur ROMs réelles (Sonic : 8 985 fermetures, couverture intacte) — reste opt-in faute de gain mesuré | matrice C2 |
| Projection C2 sur la liveness (`--ctx-live`) | NES | DuckTales 20 M pas : défaut actif 60,6 → 52,4 % connus (liveness par fenêtre 16 Kio trop grossière, éviction à l'empilage) | `docs/NES.fr.md` (0.249.83) |
| Projection C2 sur la liveness de section (`--ctx-cap --ctx-live`) | MD | Sonic 3 M pas, cap 1 : 261 571 pas / 5 184 octets exécutés contre 410 513 pas / 10 794 octets (clé grossière) — RAM/VRAM/manette hors projection = états non équivalents évincés | `docs/MD.fr.md` (0.249.84) |

## Abandonnées (avec chiffres)

| Technique | Plateforme | Pourquoi abandonné | Référence |
|---|---|---|---|
| Éviction C2 par fenêtre de 16 Kio en défaut | NES | −8 points de couverture sur DuckTales (60,6 → 52,4 %) ; revert, voir ligne opt-in ci-dessus | `docs/NES.fr.md` (0.249.83) |
| Éviction C2 par section en défaut (clé section + liveness matérielle) | MD | moitié de la couverture (10 794 → 5 184 octets exécutés) pour moitié du travail ; la liveness à l'empilage ignore RAM/VRAM/manette que la section lit | `docs/MD.fr.md` (0.249.84) |
| Inclure la manette dans la projection C2 MD | MD | chaque génération d'une boucle de lecture manette se ré-admet (la sonde D10 pose le pad sur l'état empilé) : fixture `md_ctx_cap` 15 courses au lieu de 2, 1935 pas contre 258, mêmes 8 octets exécutés | `docs/MD.fr.md` (0.249.84) |
| Dédup exacte MD en défaut | MD | 4 épingles de tests cassées (les fourches dédupliquées ne créent plus les nœuds d'arbre attendus, sondes pad) ; reportée | session 0.249.82, matrice C3 |
| Mesures du harnais GG avant correction de la boucle trame | GG | le callback de fin de trame ne posait pas `machine.stop` : le DUT courait des centaines de trames par trame de référence, les observations `$1FFE`/`$D10C` (~266 octets) étaient des artefacts ; corrigé puis re-mesuré (décalage d'une trame, résidu médian 2 octets) | `docs/Z80.fr.md` (0.249.84) |

## Jalons atteints

- **Axe 4** : la campagne multi-plateforme complète (`results/campagne-0.250.1`)
  — 1199 ROMs, zéro résumé invalide : SMS 897 (56,9 % connus en moyen),
  GG 225 (40,9 %), SG 68 (82,9 %), MD 4 (14,7 %), NES 2 (38,9 %), MSX 2
  (39,3 %), PCE 1 (21,5 %) ; 74 milliards de pas au total, observations de
  l'outil à budgets bornés par ROM (`tools/campaign_full.sh`, partage
  multi-ouvriers).

- **Axe 10** : les trois ROMs de couverture 100 % (Puzzle Boy, BattleCity,
  Master Karateka — campagnes 0.249.x) se recompilent toutes avec
  **interpreter-only 0** ; le compteur « compilé » (8601) est le plafond de
  budget flash de la carte cible, pas un repli. Réserve : le Game Genie
  (en-tête multicart incohérent).

## Leçons transversales

- **Projeter avant d'évincer** : une éviction qui compare des états sur des
  composantes qu'elle ne modélise pas (RAM, VRAM, manette) ferme des états
  non équivalents. La sémantique GB (projeter sur ce que la section a *lu*,
  appris par cours d'exécution) est la cible ; les approximations par
  fenêtre ou par blocs ont toutes deux été mesurées perdantes en défaut.
- **Évincer à l'empilage est plus grossier qu'évincer à la ré-entrée** :
  l'état d'un fork ne connaît pas encore ce que sa section lira.
- **Un harnais de fidélité se teste d'abord sur lui-même** : le premier
  signal de la divergence GG venait du harnais, pas du cœur comparé.
