# Tableau comparatif des plateformes

Observations de l'outil (machine concrète, RAM à zéro) — pas des mesures de matériel. Les budgets diffèrent d'une campagne à l'autre : le tableau compare ce qui a été exécuté, pas la difficulté intrinsèque des plateformes.

Sources : `results/compare-0.249.84/`, `results/gb-0.249.78/`.  Généré par `tools/compare_platforms.py`.

## gb

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| MARIOLAND2?????? | 524 288 | 98.0 % | 21.1 % | 17 491 541 425 | 79 173 | 7 668 498 | — |
| ZELDA??????????? | 524 288 | 98.4 % | 9.2 % | 21 731 316 147 | 1 747 | 8 965 | — |

## nes

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| ducktales-europe | 131 072 | 60.6 % | — | 1 958 188 | 1 527 | 1 488 | — |

## md

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| sonic-the-hedgehog | 524 288 | 9.0 % | — | 410 513 | 932 | 836 | 10 794 |

## sg

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| space-slalom | 8 192 | 90.5 % | 6.0 % | 45 000 000 | 785 | 969 | — |

## sms

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| hello-world-test-1 | 3 710 | 100.0 % | 0.0 % | 26 312 773 | 385 | 339 | — |

## gg

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| woody-pop | 32 768 | 74.0 % | 11.8 % | 45 000 000 | 728 | 1 828 | — |

## msx

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| aleste | 262 144 | 29.2 % | 45.7 % | 25 524 637 | 563 | 600 | — |

## pce

| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |
|---|---|---|---|---|---|---|---|
| raiden | 786 432 | 18.3 % | — | 5 000 000 | 337 | 449 | — |

## Agrégat par plateforme

| plateforme | ROMs | connu moyen |
|---|---|---|
| gb | 2 | 98.2 % |
| nes | 1 | 60.6 % |
| md | 1 | 9.0 % |
| sg | 1 | 90.5 % |
| sms | 1 | 100.0 % |
| gg | 1 | 74.0 % |
| msx | 1 | 29.2 % |
| pce | 1 | 18.3 % |
