# Audit de parité des moteurs d'exploration (0.330.0)

Audit réalisé le 14 novembre 2026 d'après [PLAN-GLM-PARITE.fr.md](PLAN-GLM-PARITE.fr.md),
par lecture du code avec collecte de preuves `fichier:ligne` et croisement avec
les tests `ctest` et les documents historiques. La matrice normative de
[COUVERTURE.fr.md](COUVERTURE.fr.md) §2.1 et la photographie de
[PLAN-SUPPORT-EXPLORATION.fr.md](PLAN-SUPPORT-EXPLORATION.fr.md) mélangent des
descriptions historiques et des mises à jour récentes ; ce document les réconcilie
par plateforme **et** par moteur (explorateur historique vs noyau générique `--core`).

## Lecture des états

- **validé** — le code existe, il est activable (ou actif par défaut), et un
  test l'épingle sur fixture. La preuve est un couple `(fichier:ligne, test)`.
- **partiel** — le code existe mais couvre une partie des sous-cas de la
  technique, ou n'est pincé par aucun test, ou n'existe que dans l'un des deux
  moteurs.
- **absent** — aucun code. Cela ne préjuge pas d'une impossibilité technique
  (cf. [PLAN-SUPPORT-EXPLORATION.fr.md](PLAN-SUPPORT-EXPLORATION.fr.md) : une
  limite d'implémentation n'est pas une impossibilité).
- **à vérifier** — aucune preuve collectée pendant cet audit (ni code trouvé
  ni absence confirmée) ; la cellule reste ouverte.

Les nombres cités sont des observations de l'outil sur les fixtures générées à
la construction (`tests/build_fixtures.py`), jamais des mesures de matériel.
Aucune ROM réelle n'a été explorée pendant cet audit : les cellules « mesures
réelles » de [COMPARATIF.fr.md](COMPARATIF.fr.md) ne sont pas réévaluées ici.

**Rectificatif E1 à vérifier par fixture :** pour l'instruction SM83
`LD A,($FF00+C)`, l'adresse effective est `$FF00 + C`. La comparaison de
`C` à `$00` dans `fork_p1` est donc cohérente avec une lecture de P1 à
`$FF00` ; remplacer `$00` par `$FF`, comme proposé plus bas, sélectionnerait
`$FFFF`. Le commentaire du code qui donne `C=$FF00` est en revanche erroné :
`C` est un registre de 8 bits. La conclusion « sonde inopérante » n'est pas
établie ; des témoins positifs et négatifs sont prévus dans le
[plan d'implémentation](PLAN-GLM-IMPLEMENTATION-PARITE.fr.md).

## 1. Cartographie des moteurs et du routage

Huit plateformes, deux familles de moteurs :

| Plateforme | Commande | Moteur historique | Noyau `--core` | Défaut |
|---|---|---|---|---|
| GB | `deepeel explore rom.gb` | `src/explore/explorer.c` (fourches, sections, sessions) | `dp_core_explore` sur `dp_machine_gb` | **noyau** ; `--legacy` / `DEEPEEL_GB_LEGACY=1` pour l'ancien (`src/deepeel.c:414`) |
| SG / SMS / GG | `deepeel z80 explore <sys> ROM` | `dp_sega_explore` (`src/platform/sega/explore.c`) | `sega_core_run` (`explore.c:2920`) | historique ; `--core` explicite (`explore.c:2982`) |
| MSX1 | `deepeel z80 explore msx ROM` | même moteur, backend `src/platform/msx/backend.c` | même `sega_core_run`, hooks clavier MSX (`src/mach/sega_machine.c:235`) | historique ; `--core` explicite |
| NES | `deepeel nes explore ROM` | `src/platform/nes/explore.c` | `dp_machine_nes` (`src/mach/nes_machine.c:117`) | historique ; `--core` explicite (`src/deepeel.c:1421`) |
| PCE | `deepeel pce explore ROM` | `src/platform/pce/explore.c` | `dp_machine_pce` (`src/mach/pce_machine.c:256`) | historique ; `--core` explicite (`src/deepeel.c:1578`) |
| MD | `deepeel md explore ROM` | `src/platform/md/explore.c` | `dp_machine_md` (`src/mach/md_machine.c:169`) | historique ; `--core` explicite (`src/deepeel.c:1887`) |
| libretro | `deepeel lr explore` | moteur propre `dp_lr_explore` (`src/mach/lr_cmd.c:304`) | **sans objet** : `DpMachine` dégradée, le noyau s'arrête sur `no_pc` (`core.c:1322`) | — |

Attention : l'auto-détection de `deepeel explore` (cartouche MD, `.pce`, iNES)
alias vers la sous-commande plateforme mais **ne propage pas `--core`**
(`src/deepeel.c:552-610`) — l'alias auto-détecté lance toujours le moteur
historique. Un utilisateur qui veut le noyau sur NES/PCE/MD doit nommer la
sous-commande explicitement.

Le noyau ne parle qu'à `DpMachine` + `DpIsa` (`src/core/dpmachine.h`) et à des
`DpPlatformHooks` optionnels (`src/explore/core.h:21-40`) : chaque ISA a ses
concrétisations (`dp_isa_sm83/z80/m6502/huc6280/68000`), chaque plateforme ses
hooks (GB `gb_machine.c:202`, Sega `sega_machine.c:289`, NES
`nes_machine.c:193`, PCE `pce_machine.c:230`, MD `md_machine.c:135`).

## 2. Matrice des explorateurs historiques

Légende : ✅ validé · ◐ partiel · ✗ absent · ? à vérifier.
Les preuves détaillées par plateforme sont en §3.

| Technique | GB | SG | SMS | GG | MSX | NES | PCE | MD |
|---|---|---|---|---|---|---|---|---|
| Fourches des deux côtés des conditionnels | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Fourches d'IRQ/NMI | ✅ 5 sources | ◐ frame+NMI (pas de ligne) | ✅ frame+ligne+NMI | ◐ frame+ligne (NMI EXT partiel) | ◐ frame (pas de NMI) | ✅ NMI+IRQ | ◐ IRQ2/timer, sans test dédié | ✅ V-int + H-int (H-int sans test) |
| Sondes manette | ✅ key/P1/événementielles | ✅ pad1/2+Pause | ✅ +Reset/TH | ✅ +TH/région | ✅ BIOS+clavier+AY | ✅ strobe+pad-full | ◐ pad+multitap, sans test dédié | ✅ 14 masques+6 boutons |
| Sondes événementielles matérielles | ✅ LY/STAT/DIV/TIMA | ✅ VDP/V,H | ✅ | ✅ | ✅ clavier | ✅ $2002 | ✗ (noyau seulement) | ✗ (noyau seulement) |
| Graines (scan de pointeurs) | ✅ | ✅ vue de boot seule | ✅ dispatch multi-banques | ✅ | ✅ (+slots PPI) | ✅ cross-bank | ✅ | ✅ S4 tables |
| Graines vecteurs/RST | ✅ forcés | ✅ (défaut on, `--no-vector-seeds`) | ✅ | ✅ | ✗ (off volontaire) | ✅ NMI/IRQ | ✅ 4 vecteurs | ✅ autovect/TRAP |
| Dispatch par vues de banques (G2) | ✅ | ✗ sans objet (non bancarisé) | ✅ | ✅ | ✅ slots | ✅ UxROM/MMC1/MMC3/MMC5 | ✅ pages $4000 | ✅ tables longwords |
| Banques et timeline | ✅ 18 variantes MBC | ✗ sans objet | ✅ 3 mappers | ✅ | ✅ 5 mappers+slots | ✅ 12 boards | ✅ MPR/TAM | ◐ Z80 seul (68000 sans mapper supporté) |
| Provenance statique | ✅ | ✅ | ✅ | ✅ | ✅ (+BIOS) | ◐ ram_src+tables, sans test dédié | ◐ ancres+inférence | ◐ taint 68000+S4 |
| Suivi de copies ROM→VRAM | ✅ 8 canaux | ✅ | ✅ | ✅ (latch 2 octets) | ✅ (+BIOS) | ✅ 6 canaux | ✅ VDC/VCE | ✅ DMA VDP+CPU+Z80 |
| Code exécuté en RAM | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ (+SMC, SRAM, Z80) |
| Fermeture statique | ✅ rounds + semis des vues ambiguës | ✅ 1 vue | ✅ vues mapper | ✅ | ✅ vues slots+banques | ✅ | ✅ vues MPR/TAM | ✅ S1/S2+second passage |
| Déduplication exacte d'états | ✅ `exact.bin` | ✗ (GB only) | ✗ | ✗ | ✗ | ✅ par défaut | ✅ par défaut | ◐ opt-in `--exact-dedup` |
| Éviction par contexte | ✅ (sans CHECK GB direct) | ✅ | ✅ | ✅ | ✅ | ◐ ctx-cap/ctx-live | ◐ idem | ◐ idem |
| Frontière, spool, reprise | ✅ lots+spool+`--resume` | ✅ spool simple | ✅ | ✅ | ✅ | ✅ lots+tree | ✅ | ✅ lots+tree |
| Sessions jouées | ✅ pool complet | ◐ play-steps (0 par défaut) | ✅ coverage-first | ✅ | ✅ | ✅ pool | ✅ checkpoints 300 tr | ✅ pool J1 |
| Rejeu TAS | ✅ vmv/vbm/bk2/log | ✅ | ✅ (+.mmv non documenté) | ✅ | ✅ | ✅ fm2/bk2 | ✅ bk2+hex | ✅ gmv/bk2+tas-dir |
| Verdicts de nécessité | ◐ required/played/forced (`speculative` jamais attribué) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Sorties | ✅ json/text/cov/map png/html | ✅ (pas de --json) | ✅ | ✅ | ✅ | ✅ | ✅ (--png) | ✅ (+heat) |
| Parallélisme | ✅ pool partagé | ◐ ≤2 workers | ✅ ≤16 | ✅ ≤16 | ◐ ≤2 | ◐ jobs=2 par phase | ◐ jobs=2 (1 si multitap) | ◐ jobs=2 par --entry |

Notes de matrice :

- **SG séparé des autres profils Z80** comme demandé : backend matériel
  distinct non bancarisé, sans RAM cartouche (`backend_sg.c:29-52`), IRQ frame
  seule + NMI Pause (`backend_sg.c:37-50`), `--mapper` refusé
  (`explore.c:1729`), taille ≤ 48 Kio (`explore.c:1732`), défauts CLI sans les
  surcharges « coverage-first » réservées à SMS/GG (`explore.c:2971-2980` :
  fork_cap 2 vs 4, pas de `--seed-banks`, queue_max 4096 vs 65536, jobs 1 vs
  tous cœurs), parallélisme plafonné à 2 (`explore.c:2382-2384`), et aucune
  campagne sur ROM SG commerciale (`Z80.fr.md:965-967`) — le profil est testé
  sur cas synthétiques seulement.
- **MSX** : graines de vecteurs BIOS désactivées volontairement
  (`explore.c:1146-1148`), sondes Pause/HALT exclues (`sega_machine.c:222`) ;
  la fermeture distingue deux slots montrant la même ROM avec des RAM
  différentes (`MSX.fr.md:135-141`).
- La colonne GB « éviction par contexte » est ✅ pour le code mais la
  couverture de test GB est faible : `evicted_ctx` n'a pas de CHECK direct dans
  `tests/test_explorer.c`, la mécanique commune n'est épinglée que côté Sega
  (`test_sega_sections.c:66,88`).

## 3. Preuves par plateforme (explorateurs historiques)

### 3.1 GB — `src/explore/` + machine m0boy

- **Forks conditionnels** : `fork_branch` (`forks.c:56-98`), élagage
  « deux côtés couverts » (`forks.c:16-43`), cap par site 4
  (`coverage_defaults.h:10`). Testé : `test_explorer.c:155,221-228,301-308,837-917`.
- **Forks IRQ** : les cinq sources VBlank/STAT/Timer/Serial/Joypad
  (`forks.c:100-117`, hook `explorer.c:1425-1426`), vecteurs semés à froid
  (`seeds.c:46-51`). Testé : `test_explorer.c:338-344`, `test_sessions.c:83-84`.
- **Sondes** : `fork_key` (`forks.c:125-168`), `fork_p1` (`forks.c:176-214`),
  `fork_event` LY/STAT/DIV/TIMA/SC/SB (`forks.c:216-319`). Testé :
  `test_explorer.c:348-355,444-452`. **Défaut trouvé** : `fork_p1` sur
  `ld a,(C)` teste `gbr->c != 0x00` (`forks.c:180`) au lieu de `!= 0xFF` — la
  sonde ne fourche jamais sur une vraie lecture de `$FF00` par `ld a,(C)`, et
  la fixture `p1poll` n'exerce que `ldh a,($00)` (`test_explorer.c:350-355`).
- **Graines** : `dp_seed_gb` (`seeds.c:32-103`) — vecteurs, scan de tables de
  pointeurs avec semis multi-banques, `--seed-banks` (`seeds.c:97-102`),
  `--entry` 32 max, cascade indirecte (`seeds.c:108-130`). Testé :
  `test_explorer.c:358-425`, `core_gbseeds.c` côté noyau.
- **Banques** : 18 variantes MBC×RAM (`CMakeLists.txt:104-105`, choix
  `m0boy.c:15-34`), timeline `DpBankEvent` (`explorer.c:1107-1180`), MBC5 boot
  banque 0 (`closure.c:81-84`). Testé : `test_explorer.c:310-327,431-435`.
- **Provenance statique** : `provenance.c:94-224,294-335,408-426` (jp hl
  prouvé, dispatchers inline, tables copiées). Testé :
  `test_explorer.c:361-436`.
- **Copies** : `copies.c` — boucles CPU (verbatim prouvé par hash FNV,
  `copies.c:144-148`), OAM DMA (`explorer.c:1060-1076`), HDMA CGB
  (`explorer.c:1034-1059`), frères de table (`copies.c:248-318`). Testé :
  `test_explorer.c:224-227,331-336,511-609`.
- **Code RAM** : `exec_outside`/`exec_wram_rom` (`explorer.c:1531-1565`),
  archive `rtcode.c:33-183`, coupes « nonsense » (`explorer.c:1379-1398`).
  Testé : `test_explorer.c:241,562-578`, ctest `wram_exec_trace`,
  `rtcode_archive`, `rtcode_generated`.
- **Fermeture** : `closure.c:95-335`, rounds CLI (`deepeel.c:1809-1847`,
  défaut 12 `explorer.c:156`). Testé : `test_explorer.c:486-524`.
- **Dédup + éviction** : dédup exact par image hash (`explorer.c:604-674`),
  éviction par contexte projeté (`ctxproj.c:13-42`, `explorer.c:1811-1831`).
  Testé : `test_explorer.c:667-766` (spool/jobs), éviction épinglée côté Sega
  seulement (voir note de matrice).
- **Frontière/reprise** : diffs, lots disque `DPFB` (`frontier_disk.c:706-806`),
  spool `DPSR/DPSZ/DPSD` + curseur persisté (`frontier_disk.c:287-347`),
  `exact.bin`/`coverage.bin`/`disc.bin`. Testé : `test_explorer.c:790-917`,
  `resume_cov.sh`, `frontier_cli.sh`.
- **Sessions/TAS** : `sessions.c` (pool 48 checkpoints, scheduler d'époques,
  priorité TAS), `tas.c` (vmv/vbm/bk2/log + profils de synchro). Testé :
  `test_sessions.c:71-214`, `tas_logkey`.
- **Sorties** : `--json/--text/--summary/--cov/--map png/--html/--rtcode`
  (`deepeel.c:997-1072`). Testé : `test_export.c`, `map_png`, `map_infer`.
- **Parallélisme** : `dp_explore` multi-explorateurs sur pool partagé
  (`explorer.c:1984-2075`). Testé : `test_explorer.c:731-766`.

### 3.2 SG / SMS / GG / MSX — `src/platform/sega/explore.c` + `src/platform/msx/`

Un seul moteur (`sega_explore_one`), backends par machine
(`backend.h:28-92`, `backend_sg.c`, `backend_gg.c`, `msx/backend.c`).

- **Forks conditionnels** : `explore.c:2023-2041` (côté non pris forcé,
  cap `fork_cap`), répétitions ED exécutées et sondées. Testé :
  `test_sega.c:147-199`, `test_msx_explore.c:36`.
- **Forks IRQ** : INT IM0/IM1/IM2 + NMI, fork « service retardé »
  (`explore.c:1976-1999`), journaux par mode
  (`explore.c:435-442,2780-2787`). Sources : frame+ligne (SMS/GG,
  `backend_gg.c:174-177`), frame+NMI Pause (SG, `backend_sg.c:37-50`),
  frame (MSX). Testé : `test_sega.c:203-211`, `test_sega_hardware.c:26-50`,
  `test_msx_explore.c:143-153`.
- **Sondes** : phase dédiée `--input-forks N` (défaut 8) sur pads, HALT-Pause
  (sg/sms, `explore.c:1975`), services BIOS MSX GTSTCK/GTTRIG/SNSMAT/CHSNS/CHGET
  (`explore.c:1026-1041`), AY R14/R15, PPI `$A9` ; sondes événementielles
  VDP_STATUS/V_COUNTER/H_COUNTER + lignes TH + région GG
  (`explore.c:1062-1138`). Noyau : `sega_event_probe`
  (`sega_machine.c:212-269`). Testé : `test_sega_inputs.c`,
  `test_sega_hardware.c`, `test_msx_explore.c inputs()`.
- **Graines** : `generate_seed_specs` (`explore.c:1642-1695`, dispatch
  multi-banques SMS/GG), `--seed-banks` (off pour SG/MSX, on pour SMS/GG),
  `--entry` avec vérification de vue (`explore.c:774-797`), vecteurs
  `--no-vector-seeds` (off pour MSX, `explore.c:1146-1148`), indirect S3
  (`explore.c:1616-1635`), décodeurs S6 sur tables y compris bancarisées
  stride 3 (`explore.c:595-745`). Testé : `test_sega_seeds.c`,
  `test_sega_indirect.c`, `test_sega_streams.c`.
- **Mappers** : Sega/Codemasters/Korean150 avec autodétection + `--mapper`
  (`backend_gg.c:182-212`), MSX plain/ASCII8/ASCII16/Konami/SCC + slots PPI
  `$A8` (`msx/backend.c:157-195`). Testé : `test_sega_mappers.c`,
  `sega_mapper_cli`, `test_msx.c`, `test_msx_closure.c mappers()`.
- **Provenance** : origines par registre/cellule = index ROM physique
  (`explore.c:135-181`), suivi de transferts purs (`src/cpu/z80/provenance.c`),
  copie verbatim ⇒ preuve (`explore.c:244-279`). Testé : `test_provenance.c`,
  `test_sega_copies.c`, `test_msx_explore.c bios_copies()`.
- **Copies** : destination résolue par `vdp_target()` (VRAM/CRAM, latch GG),
  journaux à strides (`explore.c:223-279`), BIOS MSX LDIRVM/WRTVRM/FILVRM
  (`explore.c:339-357`). Testé : `test_sega_copies.c:91,131`,
  `test_msx_bios.c:57`.
- **Code RAM** : `exec_outside` (`explore.c:2046-2066`), SMC
  (`explore.c:313-316`), coupes forcés-RAM (`explore.c:2062-2079`). Testé :
  `test_sega.c` « code RAM », `test_sega_hardware.c:138`.
- **Fermeture** : vues = capture 64 segments 1 Kio + mapper + slots MSX
  (`closure.c:105-134`), fermetures cible/bloc (`explore.c:518-534`),
  invalidations par écritures de décodeurs (`closure.c:229-238`). Testé :
  `test_sega_closure.c`, `test_z80_targets/blocks/stores/static_data.c`,
  `test_msx_closure.c`.
- **Dédup** : éviction par contexte avec liveness WRAM/ports + vues
  (`explore.c:874-943`), dédups par clé (graines, copies, nœuds) ; **pas de
  dédup exacte d'états complets** (reconnu par `Z80.fr.md:448-449`). Testé :
  `test_sega_sections.c:50-88`.
- **Frontière/reprise** : file croissante + spool `'DPSZ'` + `--resume`
  (`explore.c:470-558,1798-1816`), lots `--lot/--lot-drain`, tiers différés
  (`frontier.h:9-26`). Testé : `test_sega_spool.c`.
- **Sessions/TAS** : `--play-steps/--play-pool/--attract-frames`, Go-Explore
  avec cellules écran et `--export-state` (`explore.c:1399-1612`), TAS bk2/log/
  **Meka .mmv** (`tas.c:138-176`) avec identité SHA-1 ou MD5
  (`tas.c:209-227`), profil SMSHawk (`backend_gg.c:20-55`). Testé :
  `test_sega_play.c`, `test_sega_tas.c`, `test_tas_logkey.c`.
- **Sorties** : `--text/--map(png)/--evidence/--cov/--romdata/--summary/
  --rtcode/--tree` (`explore.c:2417-3131`), verdicts required/played/forced
  (`explore.c:944-953`). Testé : `sega_coverage`, `rtcode_sms_source`,
  `sms_ym2413_typed`, watch dumps sg/sms/gg/msx.
- **Parallélisme** : clones par `objcopy --redefine-syms`
  (`cmake/dp_sega_prefix.cmake`), 16 pour SMS/GG, 2 pour SG/MSX, fusion des
  preuves (`explore.c:2203-2269`). Testé : `test_sega_jobs.c` (jobs 2/3
  identiques au mono-cœur, clone MSX).

### 3.3 NES — `src/platform/nes/explore.c`

- **Forks conditionnels** : `explore.c:523-546`. Testé : `test_nes.c:466-474,412-419`.
- **Forks IRQ** : NMI « sans NMI » (`explore.c:697-707`) et IRQ
  (`explore.c:1075-1082`), marque `DP_NES_X_IRQ` dans le handler
  (`explore.c:457-459`). Testé via `--seed-vectors` (`test_nes.c:344-355`) ;
  le fork « sans NMI » lui-même n'est épinglé qu'indirectement (ctest
  `core_irq_fork_nes` côté noyau).
- **Sondes** : strobe `$4016` par site (`explore.c:617-674`), `--pad-full`
  256 masques, statut PPU `$2002` (`explore.c:676-695`). Testé :
  `nes_pad_probe`, `nes_pad_full`, `core_event_probes`.
- **Graines** : `--entry` 32, `--seed-banks` UxROM (`explore.c:1578-1592`),
  `--seed-vectors` NMI/IRQ (`explore.c:1594-1606`), tables S1/S3 cross-bank
  (`explore.c:1755-1790,2011-2042`). Testé : `nes_seed_banks`,
  `nes_seed_vectors`, `nes_indirect`, `nes_table*`.
- **Banques** : UxROM/MMC1/Camerica/UN1ROM/MMC3/MMC5 mode 3
  (`explore.c:1792-1938`), 12 boards acceptées (`nesmach.c:236-283`). Testé :
  `test_nes.c:88-305`, `nes_bank_dispatch{,_mmc1,_mmc3,_mmc3_m1,_mmc5}`.
- **Provenance** : `ram_src` après store PRG (`explore.c:762-781`), frères S5
  (`explore.c:2121-2174`). Testé : `nes_provenance{,_rts,_rts_split}`,
  `nes_sibling`.
- **Copies** : 6 canaux — OAM DMA `$4014` avec chaîne ROM→WRAM→OAM
  (`explore.c:826-845`), DMC (`explore.c:847-864`), boucles `$2007`, WRAM,
  SRAM, APU. Testé : `test_nes.c:64-74,220-227,477-486`, `nes_wram_copy`,
  `nes_sram_apu_copy`, `nes_oam_chain`.
- **Code RAM** : `exec_outside` + `DpRamArch` (`explore.c:506`), SMC
  (`explore.h:178`). Testé : `test_nes.c:810-834`.
- **Fermeture** : `static_closure` (`explore.c:1946-2030`). Testé :
  `test_nes.c:494-502`, `nes_static_xwindow`, `nes_second_pass`.
- **Dédup** : exacte par défaut (`explore.c:992-1011`), `--ctx-cap/--ctx-live`
  (`explore.c:1012-1027`). Testé : `test_nes.c:873-881`, `nes_ctx_cap`,
  `nes_ctx_live{,_window}`.
- **Frontière** : lots + tree DPTZ (`explore.c:1506-1663`), `--defer-tiers`
  défaut 0 (`explore.c:1389`). Testé : `test_nes.c:421-461`, `nes_spool_resume`.
- **Sessions/TAS** : FM2/BK2 (`tas.c:8-50`), 8 films, pool J1
  (`explore.c:1142-1180`). Testé : `test_nes.c:542-577,784-792`.
- **Sorties** : `deepeel.c:1518-1559`, classes `DP_NMAP_*`. Testé :
  `nes_summary_heat`, `nes_coverage_png`.
- **Parallélisme** : instance `dp_nes2_` par phase (`nes/jobs.c`). Testé :
  `nes_jobs`.

### 3.4 PCE — `src/platform/pce/explore.c`

- **Forks conditionnels** : `fork_branch` (`explore.c:506-553`, BBR/BBS bit
  inversé). Testé : `test_pce.c:126-131,160`.
- **Forks IRQ** : IRQ2/timer aux points d'unmask CLI/PLP/RTI/$1402
  (`explore.c:555-568`), vecteurs IRQ2/IRQ1/TIMER/NMI journalisés
  (`explore.h:57-61`). **Aucun test unitaire direct du fork IRQ historique**
  (ctest `core_irq_fork_pce` couvre le noyau seulement).
- **Sondes** : port pad `$1000` par nibble (`explore.c:536-549`),
  `--multitap` 5 manettes (`pcemach.c:228-238`, force jobs=1). Testé :
  `test_pce.c:106-117`, `pce_multitap_option`.
- **Graines** : `--entry ADDR`, `--seed-vectors` 4 vecteurs
  (`explore.c:1711-1721`), `--seed-banks` pages 8 Kio en fenêtre $4000
  (`explore.c:1722-1729`), dispatch G2 (`explore.c:1802`), tables `JMP (abs,X)`
  (`explore.c:1445+`). Testé : `test_pce.c:416-446`, `pce_bank_dispatch`.
- **Banques** : fenêtres MPR, timeline `DpPceBankEvent`
  (`explore.h:78-83`, `explore.c:151-165`), résolution des vues statiques TAM
  y compris peephole `lda #page; tam` (`explore.c:1209-1253`). Testé :
  `test_pce.c:78-84,466-489`, `pce_static_tam{,_multi}`.
- **Provenance** : `ram_src` + ancre `jmp (abs)` (`explore.c:830-856`),
  racines S3 (`explore.c:1319-1339`), inférence indexée abs,X/abs,Y
  (`explore.h:134-149`). Testé : `pce_provenance{,_roots}`, `pce_indirect`.
- **Copies** : 4 canaux VDC data/block-move TII/TDD/TIN/TIA/TAI/VCE
  (`explore.h:74-76`, `explore.c:168-192`). Testé : `test_pce.c:133-134,239-247`.
- **Code RAM** : `exec_outside` + ramarch pages F8-FB (`explore.c:869-874`),
  SMC (`explore.c:835`). Testé : `test_pce.c:509-515,143`.
- **Fermeture** : `static_closure` avec vues TAM (`explore.c:1202-1260`).
  Testé : `test_pce.c:466-489`.
- **Dédup** : exacte par défaut (`explore.c:415`), `--ctx-cap/--ctx-live`
  (`explore.c:416-441`). Pas de test dédié au ctx PCE (rapport compteur
  seulement, `explore.c:1984-1985`).
- **Frontière** : parking + lot + tree (`explore.c:447-463,1828-1842`),
  `--defer-tiers` défaut 0. Testé : `test_pce.c:155-219`.
- **Sessions/TAS** : BK2 PCEHawk/TurboNyma + hex par trame
  (`pce/tas.c:14-60`), checkpoints toutes les 300 trames
  (`explore.c:1028,1047`), Go-Explore (`explore.c:1064-1180`). Testé :
  `test_pce.c:315-324,395-397`.
- **Sorties** : `deepeel.c:1677-1729`, `--png` via maprender. Testé :
  `pce_summary`, `pce_coverage_png`, `core_map_huc6280`.
- **Parallélisme** : instance `dp_pce2_` par phase (`pce/jobs.c`). Testé :
  `test_pce.c:283-303` (union == série).

### 3.5 MD — `src/platform/md/explore.c`

- **Forks conditionnels** : Bcc/DBcc avec CCR forcé à minimal-bit-flips
  (`explore.c:712-732`). Testé : `test_md_explore.c:74-79,111-113,254-257`.
- **Forks IRQ** : V-int niveau 6 et H-int niveau 4 (`explore.c:503-518`).
  Testé pour V-int (`test_md_explore.c:295-303`) ; **H-int sans test dédié**.
- **Sondes** : manette `$A10002/03` → 14 masques 3+6 boutons
  (`explore.c:664-698`), `--pad-full/--pad-full6` 256×16. Testé :
  `test_md_explore.c:115-121,218-227`, `md_pad_full{,6}`. **Le moteur
  historique MD ne sonde pas le statut VDP `$C00004`** (le hook noyau le fait,
  `md_machine.c:87-122`).
- **Graines** : `--entry` 16, `--seed-vectors` G1 autovecteurs/TRAP
  (`explore.c:1779-1790`), `--bank-dispatch` G2 tables longwords
  (`explore.c:1106-1130`), tables S4 `jmp d(pc,Xn)` (`explore.c:1134-1235`),
  scan statique S1/S2 (`explore.c:979-1060`). **Pas de `--seed-banks`** (pas
  de mapper 68000 supporté, `cart_md.c:39`). Testé :
  `test_md_explore.c:359-378`, `md_entry`, `md_dispatch`, `md_s4_*`.
- **Banques** : cartouche SEGA/SMD (`cart_md.c:19-51`), SRAM overlay
  (`cart_md.c:61-65`), timeline banques **Z80 uniquement**
  (`explore.c:114,894,1989-1990`) — le 68000 n'a pas de mapper dans le
  périmètre. Testé : `test_md_explore.c:331-353`.
- **Provenance** : taint registres → `ram_prod/ram_src` (`explore.c:543-611`),
  writers An/Dn (`explore.c:617-619`), frères S5 (`explore.c:1860+`). Testé :
  `test_md_explore.c:234-251`, `md_indirect`, `md_sibling`.
- **Copies** : DMA VDP depuis regs 19-23 avec lectures de bus observées
  (`explore.c:835-860`, `mdmach.c:21-25`), copies CPU `MOVE.W` ROM→VRAM
  (`explore.c:809-825`). Testé : `test_md_explore.c:305-347`.
- **Code RAM** : `exec_outside/ram_exec/sram_exec` (`explore.c:622-631`),
  SMC (`explore.c:66,804-806`), code Z80 copié puis exécuté
  (`explore.c:862-911`). Testé : `test_md_explore.c:349-357`, `rtcode_md_*`.
- **Fermeture** : S1/S2 + second passage G4 (`explore.c:1070-1105,1813`).
  Testé : `md_second_pass`, `md_static_layer`, `md_static_bsr`, `md_sections`.
- **Dédup** : `--exact-dedup` opt-in (`explore.c:343-347`, hash
  `mdmach.c:251-269`), `--ctx-cap/--ctx-live` (`explore.c:316-341`).
  Testé : `md_exact_dedup`, `md_exact_persist`, `md_ctx_cap`, `md_ctx_live`.
- **Frontière** : parking + lot + tree F2 + `--defer-tiers` + `--unbounded`
  (`explore.c:300-408,1773-1777`, `deepeel.c:2139-2148`). Testé :
  `test_md_explore.c:138-212`, `resume_cli.sh`.
- **Sessions/TAS** : GMV Gens + BK2 (`md/movie.c:8-50`), `--tas-dir`
  (`deepeel.c:1993-2016`), pool J1 (`explore.c:170-174,1750-1766`),
  `--shoot-every` (`explore.c:199-200`). Testé : `test_md_explore.c:56-71`,
  `md_play_*`, `md_movie_shoots`, `md_bk2_movie`.
- **Sorties** : `deepeel.c:2159-2181` (+`--heat`). Testé : `md_summary`,
  `md_heat`, `md_map_*`, `md_cov_romdata`.
- **Parallélisme** : instance `dp_md2_`, split par `--entry` impair/pair
  (`md/jobs.c`). Testé : `md_jobs`, `md_play_jobs`, `platform_coverage_png`.
- **Watch** : `--watch` + tuiles référence ClownMDEmu/GPGX libretro/BizHawk
  (`deepeel.c:1866-2119`), `tools/watch_bizhawk.sh`. Testé : `md_watch_dump`,
  `lr_gpgx_trace` (cœur réel, skippé sans `DP_GPGX_TRACE_CORE`).

## 4. Le noyau `--core` : parité de la mutualisation

Le noyau (`src/explore/core.c`, point d'entrée `dp_core_explore:1315`)
implémente, **une seule fois pour les cinq ISA** :

| Technique | État | Preuve | Test |
|---|---|---|---|
| Fourches des deux côtés + cap par site | validé | `core.c:1072-1114`, cap défaut 4 | `kernel_explore` (`test_kernel.c:41-67`) |
| Résolution concolique (technique 1) | validé | `solve_cond` `core.c:560-605` | `core_solve` |
| Forcé à contamination (1.b) | validé | coupes indirect/ret `core.c:998-1003` | `core_taint_cut` |
| Forks IRQ + jitter (technique 9) | validé (5 plateformes) | `core.c:1046-1071` + hooks `irq_alt` par plateforme | `core_irq_fork{,_nes,_pce}`, `core_irq_jitter` |
| Sondes événementielles (technique 2) | validé (5 plateformes) | `core.c:1023-1044` + hooks `event_probe` | `core_event_probes{,_sm83,_68000,_huc6280}`, `core_pad_probes`, `core_pause_probe` |
| Sessions held-pad / attract / play | validé | `core.c:765-871,1463-1490` | `core_sessions` |
| TAS + jitter + dump DSY | validé | `core.c:1491-1506,824-839` | `core_options` ; profils de synchro non appliqués en noyau (GB `deepeel.c:657-694`) |
| Graines `--seed-ram` (technique 3) | validé | `core.c:1450-1462` | `core_seed_ram` |
| Graines pointeurs GB | validé (GB only) | `core_gbseeds.c:17-44` | exécuté par le CLI GB ; pas de test isolé dédié — à vérifier |
| `seed_banks` (écriture $2000) | partiel — **GB only** | `core.c:1439-1441` | aucun — absent sur les autres plateformes |
| Vecteurs Sega `--seed-vectors` | validé | `explore.c:2936-2938` | `core_vector_seeds` |
| Rejeu par index (technique 7) | validé (5 ISA) | `core.c:913-939,682-720` | `core_index_replay{,_6502,_68000,_sm83,_huc6280}`, `core_index_max_sm83` |
| Octets RNG (technique 4) | validé | `core.c:941-970` | `core_rng_bytes{,_6502,_68000}` |
| Signatures `ld a,(nn); cp n` (technique 3) | validé (5 ISA) | `core.c:644-679,971-997` | `core_learn_sig{,_6502,_68000}` |
| Décompresseurs (technique 6) | validé | `core.c:162-178,444-446` | `core_decompress{,_sm83,_huc6280,_68000,_6502}` |
| Cellules Go-Explore sémantiques (technique 8) | validé | `core.c:468-478,1100-1103` | `core_cells{,_auto_sm83,_auto_z80,_auto_6502,_auto_huc6280,_auto_68000}` |
| Ordonnancement nouveauté + score de site (technique 5) | validé | `core.c:1276-1304` | `core_novelty`, `core_site_score` |
| Fermeture statique | partiel — marches sur l'image ROM supposent la géométrie banques×$4000 et les tables jp stride 3 sont sm83/z80 seulement (`core.c:285,319,344`) | `closure_walk:228-275`, `closure_jp_table:343-388` | `core_closure_{sm83,huc6280,68000,6502,z80}` (sans tables bancarisées hors sm83/z80) |
| Copies ROM→RAM génériques | validé | `obs_read/obs_write` `core.c:432-465` | `core_copies` |
| Code en RAM + ramarch + coupes forcés-RAM | validé | `core.c:891-911` | `core_exec_outside`, `core_forced_ram_cut` |
| Déduplication | **différente par politique** : cellules (cible, RAM) + probeset + « deux côtés couverts », **pas de dédup exacte d'états** (celle des historiques NES/PCE/MD/GB) | `core.c:468-478` | `core_cells*` |
| Frontière + spool + `--resume` | validé (formats `DFR1`/`cells.bin`/`coverage.bin`/`disc.bin`) | `core.c:1380-1577`, spool `core.c:499-551` | `frontier_kernel.sh`, `core_options` |
| File/`--queue-max` | validé | `core.c:114-141`, spool au-delà | compteur `sat_queue` rapporté |
| Parallélisme `--jobs` + adaptatif | validé | `core.c:1242-1372`, contrôleur `1152-1214` | `core_jobs` |
| Sorties cov/map/summary/text/rtcode | validé | `dp_core_write_cov:1634-1648` (index banque $4000 GB only), `core_map.c`, `dp_core_report:1584-1632` | `core_map_{sm83,z80,m6502,huc6280,68000}`, `core_residual` |
| `--unbounded` | validé (lève max_total/max_path_steps/fork_site_cap) | `core.c:43,1317` | aucun dédié — à vérifier (le contrat du dépôt « l'unique arrêt est l'épuisement ou SIGINT » n'est pas épinglé) |

**Cavités du noyau signalées à l'audit** :

1. `no_pc` sur libretro niveau A : par conception (`dpmachine.h:211-213`),
   le noyau ne remplace pas le driver libretro.
2. Le commentaire `explore.c:2912-2913` (« MSX keeps its own engine until its
   hooks exist ») est **obsolète** : le noyau gère MSX depuis 0.287.0.
3. Le nom « screen cells » dans `dp_core_report` (`core.c:1608`) est un reste
   GB : les cellules du noyau sont les paires (cible, RAM), pas des cellules
   d'écran.
4. Pas de sondes « nonsense » : l'ancien moteur GB coupe l'exécution dans
   echo/OAM/donnée verbatim/pile hors WRAM (`explorer.c:1379-1398`) ; le noyau
   n'a pas d'équivalent et peut marquer du padding comme code naturel sur les
   petites fixtures (observé par la PoC §7 : 32447 octets « naturels » sur la
   fixture 32 Kio `fix_irq`). Différence de politique, pas une perte, mais elle
   gonfle le « natural » si l'on compare tel quel.
5. Les cinq backends exposent `clone`/`attach` pour `--jobs`, sauf vérification
   fine : GB (`gb_machine.c:222`), Sega (`sega_machine.c:141-163`), NES
   (`nes_machine.c:100-113`), PCE (`pce_machine.c:236-249`), MD
   (`md_machine.c:138-160`). État : validé par `core_jobs` (backend Sega) ;
   clones NES/PCE/MD/GB en noyau sans test dédié — à vérifier.

## 5. Contradictions documents ↔ code (avec corrections proposées)

Aucune de ces contradictions ne doit être lue comme « la technique est
impossible » — ce sont des retards de documentation ou des défauts de code à
traiter individuellement.

### 5.1 `docs/GB.fr.md` (non corrigé ici : le fichier est réservé par une autre tranche)

| # | Document | Code | Correction proposée |
|---|---|---|---|
| 1 | « `--closure-rounds` (défaut 4) » (`GB.fr.md:332`) | défaut **12** (`explorer.c:156`) | mettre la doc à 12 |
| 2 | « `--tas-ckpt-max` (défaut 1024) » (`GB.fr.md:350`) | **4096** (`sessions.c:54`) ; la doc se contredit en `GB.fr.md:4315` (4096) | corriger l'entrée 350 |
| 3 | snapshots TAS « min(frames/1024, 30 s × 60) » et « `--tas-ckpt-secs` à 5 » (`GB.fr.md:349-350,4315`) | **10 s** (`coverage_defaults.h:9`) | une seule valeur de doc, alignée sur le code |
| 4 | « `--tas-rng-share` défaut 2 » / « `--tas-rng-frames` défaut 3600 » (`GB.fr.md:4308-4309`) | **1** (`sessions.c:56`) / **2000** (`coverage_defaults.h:6`) | corriger |
| 5 | « l'ancien moteur reste le défaut » (`GB.fr.md:4623,5069`) | **le noyau est le défaut GB** (`deepeel.c:414`, `--legacy`/`DEEPEEL_GB_LEGACY=1`) ; les entrées tardives de la même page le reconnaissent | corriger les entrées antérieures |
| 6 | « `ld a,(C)` avec C=$FF00 » (`GB.fr.md:136`) | le code teste `c != 0x00` (`forks.c:180`) — **défaut de code**, la doc décrit l'intention | corriger le code : `c != 0xFF` ; ajouter une fixture `ldh (C),a`-style avec C=$FF00 |
| 7 | « 10 instances / 17 variantes » (`GB.fr.md:110-113`) | **18 machines / 18 variantes** (`CMakeLists.txt:101,104`) | corriger |
| 8 | verdict `speculative` présenté dans la taxonomy | l'énumération existe (`explorer.h:117`) mais n'est **jamais attribuée** ; seuls required/played/forced sortent (`explorer.c:1890-1898`) — les plateformes NES/PCE/MD ont un vrai `speculative` (`nes_speculative`, `pce_speculative`, `md_speculative`) | attribuer `DP_V_SPECULATIVE` sur GB (sections forcé-seulement hors flux) ou retirer la valeur |

### 5.2 `docs/Z80.fr.md`

| # | Document | Code | Correction proposée |
|---|---|---|---|
| 9 | « deux instances au plus », « MSX n'a pas de clone » (§ jobs, ~l.349-371 ; table ~l.245) | `max_jobs = sms_gg ? 16 : 2` (`explore.c:2382-2384`), clones gg3..gg16 (`CMakeLists.txt:474-490`), **clone MSX** `dp_msx_backend2` (`CMakeLists.txt:528-546`) et `test_sega_jobs.c:60-70` le vérifie | réécrire la section jobs |
| 10 | table des défauts (~l.223-246) : `--fork-cap 4`, `--seed-banks` coupé, `--queue` max 4096 | défauts API 2 (`explore.c:1142`), **on** SMS/GG (`explore.c:2977`), queue_max **65536** SMS/GG ; la page dit l'inverse plus haut (~l.41-46) | scinder la table par profil (SG/MSX vs SMS/GG) |
| 11 | « MSX n'est pas supporté (hooks à venir) » (`Z80.fr.md:1271-1273`, jalon 0.254.0) | le noyau MSX existe depuis 0.287.0 (`sega_core_run:2920-2935`, `dp_machine_sega_set_msx`) | garder la note historique mais pointer la mise à jour |
| 12 | TAS « .bk2 + log texte » seulement (`Z80.fr.md:302-309`) | **Meka .mmv** accepté aussi (`tas.c:138-176`) | documenter .mmv |
| 13 | (d'après l'audit) dédup exacte d'états absente côté Z80 | reconnu implicitement (`Z80.fr.md:448-449`) | documenter explicitement l'écart de politique avec GB/NES/PCE |

### 5.3 `docs/MD.fr.md`

| # | Document | Code | Correction proposée |
|---|---|---|---|
| 14 | « BK2 Mega Drive n'est pas encore pris en charge » (`MD.fr.md:50`) | `movie.c:8-50` charge BK2 ; `deepeel.c:2043-2049` vérifie le MD5 | corriger |
| 15 | « pas de fermeture statique, Z80 non exploré, pas de sessions jouées, pas de GUI » (`MD.fr.md:187-189`) | fermeture S1/S2+G4 (`explore.c:979-1235`), Z80 exploré (`explore.c:862-935`), sessions (`explore.c:1741-1766`), `--watch` (`deepeel.c:2035`) | réécrire la section limites |
| 16 | « il n'y a pas encore de déduplication par contexte » (`MD.fr.md:152`) | `--exact-dedup`/`--ctx-cap/--ctx-live` (`explore.c:316-347`) ; `MD.fr.md:233` contredit déjà la ligne 152 | corriger la ligne 152 |
| 17 | « quatre instantanés bornés (Start, A, droite, A+droite) » (`MD.fr.md:166-170`) | **14 masques** dont 6 boutons (`explore.c:666-667`, `test_md_explore.c:116`) | corriger |
| 18 | « journal des changements de banque » généralisé (`MD.fr.md:27`) | vrai pour la passe linéaire (`md_trace.c:139-142`) ; l'explorateur à fourches ne journalise que les banques **Z80** (`explore.c:114`) | distinguer les deux passes |

### 5.4 `docs/NES.fr.md`

| # | Document | Code | Correction proposée |
|---|---|---|---|
| 19 | « NROM et UxROM — mapper 0 et 2 seulement, les autres refusées » (`NES.fr.md:3-5,25-27`) | 12 boards acceptées : 0,1,2,3,4,5,7,11,66,71,94,180 (`nesmach.c:238-283`) ; dispatch G2 MMC1/MMC3/MMC5 testé (`test_nes.c:124-305`) | réécrire (retard d'au moins deux jalons) |
| 20 | Usage réduit à quelques options (`NES.fr.md:72-77`) | l'usage réel expose `--summary --cov --romdata --evidence --seed-banks --seed-vectors --bank-dispatch --second-pass --lot/--resume --jobs --ctx-cap --novelty` (`deepeel.c:1405-1408`) | compléter |

### 5.5 `docs/PCE.fr.md`

| # | Document | Code | Correction proposée |
|---|---|---|---|
| 21 | Usage « `--text --map --png` » (`PCE.fr.md:12-13`) | l'usage réel expose TAS/play/novelty/lot/jobs/multitap/etc. (`deepeel.c:1568-1571`) | compléter |
| 22 | copies VDC « port de données ou transfert de bloc » (`PCE.fr.md:53-55`) | 4 canaux dont **VCE palette** non documenté (`explore.h:74-76`) | ajouter VCE |

### 5.6 Contradictions internes au code (hors docs)

| # | Objet | Détail | Action proposée |
|---|---|---|---|
| 23 | Sonde P1 GB `forks.c:180` | `gbr->c != 0x00` au lieu de `!= 0xFF` : la sonde `ld a,(C)` ne fonctionne pas ; non détectée par les tests | correctif + fixture (test minimal §8, E1) |
| 24 | `DP_V_SPECULATIVE` mort sur GB (`explorer.h:117`) | jamais attribué (`explorer.c:1890-1898`) | attribuer ou retirer |
| 25 | Commentaire obsolète `explore.c:2912-2913` | le noyau MSX existe | mettre le commentaire à jour |
| 26 | « screen cells » (`core.c:1608`) | nom hérité trompeur pour les cellules (cible, RAM) | renommer le libellé du rapport (incrément de version si visible) |

## 6. Banc de parité reproductible (proposition)

### 6.1 Socle existant réutilisable (déjà dans le dépôt)

- **Fixtures générées au build** (`tests/build_fixtures.py`, ~130 fixtures,
  une par comportement et par ISA) ; aucune ROM dans le dépôt (vérifié :
  `git ls-files` ne contient aucun binaire ROM/movie).
- **Comparaison in-process d'ensembles d'octets** : `tests/test_lr_parity.c`
  compare déjà les `DP_XF_START` de deux moteurs octet par octet sur 11
  fixtures GB (`lr_parity`, CMakeLists.txt:389-392) — historique vs driver
  libretro, avec exclusions explicites (vecteurs <$100, padding).
- **Comparaison d'états par trame, par octet, avec plages ignorées** :
  `src/core/desync.c` (format DSY1, `dp_dsy_compare:70-127`) et ses
  producteurs génériques (`tools/lr_dsy.c`, `tools/nes_dsy.c`,
  `tools/pce_bridge_dsy.c`, `tools/bizhawk_dsy`).
- **Ensembles de couverture persistés** : `--cov` (`bank addr flags hits`,
  drapeaux 1=naturel, 5=forcé, 8=donnée), `--romdata`, `--summary` JSON
  (`classes` = rom_size, `counters`), `coverage.bin` OR-é à la reprise, carte
  classes 1 char/octet (`--map`), comparateur de résidus
  (`tools/compare_residual.py`).
- **États différés et compteurs** : `defer_by[DP_R_*]`, `sat_*` du noyau,
  `frontier stats/drain`, arbres `--tree`, reprise testée « reprise ==
  continu » côté GB (`test_sessions.c:100-155`) et Sega
  (`test_sega_spool.c:62`).
- **Parité contre de vrais cœurs** (hors ctest obligatoire) :
  `lr_gambatte_parity` et `lr_gpgx_trace.sh`, plus `tools/trace_cmp.py`,
  `tools/replay_compare.py`, `tools/dsy_cmp.py`.

### 6.2 Ce qui manque

**Aucun test ne compare systématiquement moteur historique vs noyau `--core`
sur une même fixture** : les tests `core_*` vérifient le noyau contre lui-même,
les tests de plateforme vérifient l'historique contre lui-même ; `lr_parity`
ne couvre que GB (historique vs libretro). Le banc proposé comble cela :

1. **Protocol** — pour chaque plateforme et chaque fixture applicable :
   mêmes fixtures générées, mêmes graines explicites (`--entry`/vecteurs),
   mêmes budgets (`--max-total`, `--max-steps`, `--fork-cap`), mêmes
   techniques désactivées quand un seul moteur les possède (comme dans la
   PoC §7), puis exécution des deux moteurs avec `--cov` + `--summary` +
   `--map`.
2. **Comparaison par octet, pas par total** — union/intersection/diff des
   ensembles d'octets exécutés, **par source** (modèle §4 de
   COUVERTURE.fr.md : `DP_SRC_FORK_NATURAL/FORCED/CLOSURE/COPY/...`) ;
   comparaison des classes de carte (`--map`), des états différés
   (`defer_by`, `sat_*`, `frontier stats`) et des compteurs de saturation.
3. **Classification des différences** — chaque écart est étiqueté :
   - *différence de politique attendue* (ex. : concolique noyau ⇒ des côtés
     forcés historiques deviennent naturels ; coupes « nonsense » absentes du
     noyau ⇒ sur-couverture ; cellules vs dédup exacte) ;
   - *perte de couverture* (un octet prouvé par un moteur et par aucun
     autre) — interdit par le contrat, doit être 0 ;
   - *faux positif candidat* (un octet couvert par l'un et manifestement du
     padding pour l'autre) — à examiner avec les témoins négatifs.
4. **Témoins négatifs** — les fixtures `*counterexample`/`nsecho/nsdata`/
   `copyx` (non-verbatim) déjà présentes doivent rester non classées par les
   deux moteurs.
5. **Reproductibilité** — commandes, version, graines et empreintes des
   fixtures consignées dans le rapport du banc (JSON), comme le demandent
   [PLAN-SUPPORT-EXPLORATION.fr.md](PLAN-SUPPORT-EXPLORATION.fr.md) §Plan 2.

Forme recommandée : un script `tools/parity_bench.py` (non écrit dans cette
tranche) pilotant la CLI `deepeel` et comparant les sorties `--cov/--map/
--summary` ; la PoC §7 en est le noyau de preuve in-process.

### 6.3 Critères d'acceptation du banc

- Sur chaque fixture : `missing == 0` dans les deux sens après application
  de la liste des différences de politique admises.
- Aucun état perdu : `defer_resumed + defer_pending == defer_total`, spools
  saturés comptés, reprise == continu sur les fixtures de reprise.
- La saturation est rapportée, jamais silencieuse (`sat_*` ≥ 1 quand un mur
  est touché).

## 7. Preuve de concept réalisée : `tests/test_audit_parity.c`

Nouveau fichier propre à l'audit (ctest `audit_parity`), **GB seulement**,
comme le permet le livrable 4 du plan. Il fait tourner, sur la même fixture
et à budgets égaux (2 M pas, 1000 pas/chemin, cap de fourches 4), l'explorateur
historique (`dp_seed_gb` + `dp_explore`) et le noyau (`dp_core_explore` sur
`dp_machine_gb` avec les hooks GB), puis compare les ensembles d'octets
exécutés dans l'espace d'index commun `dp_rom_index` :

- les techniques que le noyau possède seul (concolique, RNG, signatures,
  index, décompresseurs, cellules, nouveauté) sont désactivées pour que les
  deux runs exercent le cœur commun ;
- le contrat épinglé est la **direction sans perte** : tout octet prouvé par
  l'historique (naturel, forcé, IRQ) est couvert par le noyau ;
- les différences de nature (un forcé historique prouvé naturel par le noyau)
  et la sur-couverture du noyau sont comptées et affichées, jamais
  silencieuses.

Résultats observés (observations de l'outil sur fixtures, 0.330.0 + PoC) :

```
audit-parity branch: legacy exec 63 (nat 31 forced 32 irq 1), kernel exec 170
  (nat 123 forced 47), missing 0, forced->natural 12, kernel extra (nat 92 forced 27)
audit-parity irq: legacy exec 27 (nat 15 forced 12 irq 3), kernel exec 32447
  (nat 32447 forced 0), missing 0, forced->natural 12, kernel extra (nat 32432 forced 0)
audit_parity: ok
```

Ce que ces mesures montrent (et que la matrice normative ne disait pas) :

1. **Aucune perte** sur les deux fixtures : le noyau couvre tout ce que
   l'historique prouve, y compris les octets d'IRQ.
2. **Le forcé historique devient naturel** : 12 octets par fixture sont
   prouvés naturellement par le noyau là où l'historique n'avait qu'un fourché
   — c'est la promesse de la concolique/du parcours plus large, mesurée ici
   pour la première fois sur fixture.
3. **Sur-couverture du noyau** : sans les coupes « nonsense » de
   l'historique (§4, cavité 4), le noyau exécute le padding comme code
   (32447/32450 octets sur `fix_irq`). Ce n'est pas une perte mais cela
   **dégrade la valeur des verdicts noyau** sur les petites ROMs : à traiter
   avant de comparer des « known_pct » entre moteurs.

## 8. Écarts classés par impact, dépendances et test minimal

| # | Écart | Impact | Dépendances | Test minimal |
|---|---|---|---|---|
| E1 | Sonde P1 `ld a,(C)` inopérante (`forks.c:180`) | sonde GB fausse sur les jeux qui lisent le pad par `ld a,(C)` ; la doc le promet | aucune | fixture GB : `ld a,(C)` avec C=$FF00 en polling, CHECK `forks_p1 ≥ 1` |
| E2 | Pas de coupes « nonsense » dans le noyau (padding/données/echo/pile) | verdicts noyau gonflés (padding compté « code naturel ») | aucune | la PoC §7 montre déjà le phénomène ; test : fixture padding, CHECK que le noyau ne classe pas le padding en NATURAL |
| E3 | `DP_V_SPECULATIVE` jamais attribué sur GB | verdict GB incomplet vs NES/PCE/MD | aucune | réutiliser le schéma de `nes_speculative` sur fixture GB à section forcé-seulement |
| E4 | `seed_banks`/`--seed-banks` noyau GB-only (écriture $2000) | les autres plateformes perdent le semis par banque en `--core` (Sega le rétablit au niveau CLI) | hooks de banque par plateforme | fixture z80 : `--core --seed-banks` atteint chaque vue — **traité (2026-11-14, 0.340.0)** : hook `DpMachine.bank_view` (GB `$2000`/`$3000`, Sega `seed_window`, NES registre du mapper dont MMC3/MMC5 par fentes 8 Ko, PCE pages MPR à `$4000`), la boucle de semis vit dans `core.c` une fois ; refus comptés (`seed_banks_absent`, imprimé) ; MD sans objet (pas de mapper 68000) |
| E5 | Fermeture noyau : tables jp stride 3 sm83/z80 seulement, image ROM supposée banques×$4000 | provenance statique bancarisée absente en noyau 6502/huc6280/68000 | géométrie de fenêtres par plateforme dans `DpMachine` | fixture NES/PCE : table {lo,hi,bank} atteinte via `--core` |
| E6 | Dédup exacte d'états absente du noyau (par politique) | à mesurer : risque de ré-exploitations sur grandes ROMs | banc §6 | mesure avec/sans cellules sur fixture anti-boucle |
| E7 | Forks IRQ historiques PCE sans test unitaire ; H-int MD ; NMI NES indirect | régressions possibles non détectées | aucune | CHECK `irq_forks ≥ 1` dans `test_pce.c`/`test_md_explore.c` |
| E8 | Éviction par contexte GB sans CHECK direct | idem GB | aucune | CHECK `evicted_ctx ≥ 1` sur fixture à ré-entrée (`test_sega_sections.c:66` porté) |
| E9 | Écarts de documentation 5.1–5.5 (18 entrées) | pièges de lecture, défauts CLI erronés | GB.fr.md réservé — à faire par la tranche qui le tient | `audit_matrix` étendu aux défauts CLI |
| E10 | `--core` non propagé par l'auto-détection (`deepeel.c:552-610`) | l'utilisateur croit comparer les moteurs et compare le même moteur | aucune | test CLI : `explore` auto-détecté + `--core` refuse ou route |
| E11 | `--unbounded` noyau sans test dédié au contrat de frontière (rien perdu, SIGINT seul arrêt) | contrat du dépôt non épinglé côté noyau | aucune | test : saturation + reprise, `spool_left` et états jamais perdus |
| E12 | Profils de synchro TAS non appliqués en noyau GB (`deepeel.c:657-694`) | TAS noyau potentiellement désynchronisé des enregistreurs | profils `dp_tas_tune_machine` | désync sur fixture film avec profil |

Sans objet / confirmés absents par conception : noyau sur libretro niveau A
(`no_pc`, §4) ; `--seed-banks` MD (pas de mapper 68000) ; vecteurs MSX (désactivés
volontairement) ; GUI z80 (hors périmètre assumé, `Z80.fr.md:14-16`).

## 9. Cellules sans preuve et tests non exécutés pendant cet audit

- **Sans preuve (à vérifier)** : graines pointeurs GB du noyau sans test
  isolé dédié ; `--unbounded` noyau ; clones noyau NES/PCE/MD/GB ; comportement
  du noyau sur ROM réelle NES/PCE/MD/Sega (aucune ROM explorée ici) ;
  `--seed-vectors` par défaut sur SG (défauts SG = API, `seed_vectors` à
  vérifier dans `dp_sega_options`).
- **Tests non exécutés** : `lr_gpgx_trace` (requiert `DP_GPGX_TRACE_CORE`),
  `lr_gambatte_parity` (requiert `DEEPEEL_GAMBATTE_LR`), `gui` (SDL2),
  `mapwin_cli`/`maprender_cv` (dépendances graphiques) — skippés comme en
  campagne habituelle ; ils n'entrent donc pas dans les preuves « validé ».
- Les pourcentages de campagne cités dans [COMPARATIF.fr.md](COMPARATIF.fr.md)
  et [Z80.fr.md](Z80.fr.md) ne sont pas re-mesurés ici ; ce document ne les
  confirme ni ne les infirme.

## 10. Suite

1. Traiter E1 (correctif + fixture) et E3 (verdict GB) — petits, autonomes.
2. Étendre la PoC au banc complet (`tools/parity_bench.py`, §6.2) : GB legacy
   vs noyau d'abord (les deux moteurs sont câblés), puis Sega, NES, PCE, MD.
3. Porter les corrections de documentation 5.2–5.5 (GB.fr.md est réservé).
4. Mettre à jour la matrice §2.1 de COUVERTURE.fr.md après révision par le
   propriétaire — ce document est l'entrée d'audit, pas la norme.
