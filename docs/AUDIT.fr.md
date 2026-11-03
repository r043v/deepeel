# Audit de la matrice §2.1

Généré par `tools/audit_matrix.py` à partir de `docs/COUVERTURE.fr.md`. Les marques
sont les affirmations du document ; ce script en audite la forme et les totaux, il ne
remesure pas l'outil.

39 lignes de technique, 6 plateformes.

| Plateforme | ✅ fait | ◐ partiel | ✗ absent | — sans objet |
|---|---|---|---|---|
| GB | 34 | 1 | 3 | 1 |
| SMS/GG | 32 | 1 | 4 | 2 |
| MSX | 32 | 1 | 4 | 2 |
| NES | 7 | 8 | 22 | 2 |
| PCE | 15 | 7 | 16 | 1 |
| MD | 8 | 7 | 23 | 1 |

Cellules ouvertes (◐ ou ✗) : 97.

| # | Technique | Plateforme | État |
|---|---|---|---|
| D1 | Marquage exec natural/forced/entry/irq/start | NES | ◐ |
| D1 | Marquage exec natural/forced/entry/irq/start | PCE | ◐ |
| D1 | Marquage exec natural/forced/entry/irq/start | MD | ◐ |
| D3 | Accès registres par registre (`DpIoStat`) | NES | ◐ |
| D3 | Accès registres par registre (`DpIoStat`) | PCE | ◐ |
| D3 | Accès registres par registre (`DpIoStat`) | MD | ◐ |
| D4 | Copies par boucle CPU | NES | ✗ |
| D10 | Sondes de lecture manette | NES | ✗ |
| D10 | Sondes de lecture manette | MD | ◐ 4 |
| D12 | Cibles indirectes observées | NES | ✗ |
| D12 | Cibles indirectes observées | PCE | ✗ |
| D12 | Cibles indirectes observées | MD | ✗ |
| D13 | Coupes de non-sens | NES | ◐ illégal |
| D13 | Coupes de non-sens | PCE | ◐ illégal |
| D13 | Coupes de non-sens | MD | ◐ faute |
| D14 | Chien de garde HALT, glissade de padding | NES | ✗ |
| D14 | Chien de garde HALT, glissade de padding | PCE | ✗ |
| D14 | Chien de garde HALT, glissade de padding | MD | ✗ |
| S1 | Scan padding + tables de pointeurs | NES | ✗ |
| S1 | Scan padding + tables de pointeurs | MD | ✗ |
| S2 | Fermeture statique de flot | NES | ✗ |
| S2 | Fermeture statique de flot | PCE | ◐ fenêtre MPR |
| S2 | Fermeture statique de flot | MD | ✗ |
| S3 | Suivi de constantes / provenance | GB | ◐ HL/A |
| S3 | Suivi de constantes / provenance | NES | ✗ |
| S3 | Suivi de constantes / provenance | PCE | ✗ |
| S3 | Suivi de constantes / provenance | MD | ◐ témoin An |
| S4 | Tables de sauts depuis l'opérande | GB | ✗ |
| S4 | Tables de sauts depuis l'opérande | NES | ✗ |
| S4 | Tables de sauts depuis l'opérande | MD | ✗ |
| S5 | Frères de copie | NES | ✗ |
| S5 | Frères de copie | PCE | ✗ |
| S5 | Frères de copie | MD | ✗ |
| S6 | Rejeu de décodeurs sur tables de flux | GB | ✗ |
| S6 | Rejeu de décodeurs sur tables de flux | SMS/GG | ✗ |
| S6 | Rejeu de décodeurs sur tables de flux | MSX | ✗ |
| S6 | Rejeu de décodeurs sur tables de flux | NES | ✗ |
| S6 | Rejeu de décodeurs sur tables de flux | PCE | ✗ |
| S7 | Inférence de voisins à la carte | NES | ✗ |
| S7 | Inférence de voisins à la carte | MD | ✗ |
| G1 | Graines reset/RST/IRQ | NES | ◐ reset |
| G1 | Graines reset/RST/IRQ | PCE | ◐ reset |
| G1 | Graines reset/RST/IRQ | MD | ◐ reset |
| G2 | Scan de dispatch par vues de banques | NES | ✗ |
| G2 | Scan de dispatch par vues de banques | PCE | ✗ |
| G2 | Scan de dispatch par vues de banques | MD | ✗ |
| G3 | `--seed-banks`, `--entry` | NES | ◐ `--entry` ok, `--seed-banks` absent |
| G3 | `--seed-banks`, `--entry` | PCE | ✗ |
| G3 | `--seed-banks`, `--entry` | MD | ✗ |
| G4 | Semis indirect + budgets en cascade | NES | ✗ |
| G4 | Semis indirect + budgets en cascade | PCE | ✗ |
| G4 | Semis indirect + budgets en cascade | MD | ✗ |
| C1 | Sections, liveness, verdicts | NES | ◐ req/forced |
| C1 | Sections, liveness, verdicts | PCE | ◐ req/forced |
| C1 | Sections, liveness, verdicts | MD | ✗ |
| C2 | Éviction par contexte | NES | ✗ |
| C2 | Éviction par contexte | PCE | ✗ |
| C2 | Éviction par contexte | MD | ✗ |
| C3 | Dédup exacte hors horloges | MD | ✗ |
| F1 | File croissante + spool + `--resume` | NES | ✗ |
| F1 | File croissante + spool + `--resume` | PCE | ✗ |
| F1 | File croissante + spool + `--resume` | MD | ✗ |
| F2 | Arbre de choix persistant | NES | ✗ |
| F2 | Arbre de choix persistant | PCE | ✗ |
| F2 | Arbre de choix persistant | MD | ✗ |
| F3 | `--jobs` par instances clonées | NES | ✗ |
| F3 | `--jobs` par instances clonées | PCE | ✗ |
| F3 | `--jobs` par instances clonées | MD | ✗ |
| J1 | Sessions jouées multi-thread + pool | SMS/GG | ◐ |
| J1 | Sessions jouées multi-thread + pool | MSX | ◐ |
| J1 | Sessions jouées multi-thread + pool | NES | ◐ xorshift |
| J1 | Sessions jouées multi-thread + pool | PCE | ◐ |
| J1 | Sessions jouées multi-thread + pool | MD | ✗ |
| J2 | Rejeu TAS prioritaire + shoots | MD | ◐ GMV |
| J3 | Identité ROM des films | NES | ✗ |
| J3 | Identité ROM des films | PCE | ✗ |
| J3 | Identité ROM des films | MD | ✗ |
| J4 | Go-Explore (cellules écran) | GB | ✗ |
| J4 | Go-Explore (cellules écran) | SMS/GG | ✗ |
| J4 | Go-Explore (cellules écran) | MSX | ✗ |
| J4 | Go-Explore (cellules écran) | NES | ✗ |
| J4 | Go-Explore (cellules écran) | MD | ✗ |
| J5 | Mode humain, export `.sta` | SMS/GG | ✗ |
| J5 | Mode humain, export `.sta` | MSX | ✗ |
| J5 | Mode humain, export `.sta` | NES | ✗ |
| J5 | Mode humain, export `.sta` | PCE | ✗ |
| J5 | Mode humain, export `.sta` | MD | ✗ |
| O1 | `.map` texte, PNG, JSON, heat | NES | ◐ |
| O1 | `.map` texte, PNG, JSON, heat | MD | ✗ dump brut |
| O2 | `.cov`, romdata, `--evidence` | NES | ✗ |
| O2 | `.cov`, romdata, `--evidence` | PCE | ✗ |
| O2 | `.cov`, romdata, `--evidence` | MD | ✗ |
| O3 | `--watch` (SDL2) | SMS/GG | ✗ |
| O3 | `--watch` (SDL2) | MSX | ✗ |
| O3 | `--watch` (SDL2) | NES | ✗ |
| O3 | `--watch` (SDL2) | PCE | ✗ |
| O3 | `--watch` (SDL2) | MD | ✗ |

Problèmes de forme : aucun.
