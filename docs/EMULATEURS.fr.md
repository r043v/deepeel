# Émulateurs source des films TAS : liste des révisions suivies

État au 2026-11-25. À mettre à jour à chaque rejeu qui change un statut. Le nom d'émulateur est celui que donne
l'API TASVideos (`deepeel tasdb emu`), jamais l'en-tête du film. Tout ce qui suit est une observation de l'outil,
pas une comparaison avec l'émulateur d'origine, sauf mention contraire.

Statuts : **pris en charge** (un film rejoué jusqu'à la fin du jeu, écran vérifié) ; **partiel** (synchro établie sur
une partie seulement) ; **non vérifié** (rejoué ou rejouable, fin non contrôlée) ; **désynchronisé** ; **désactivé**
(la campagne n'ouvre pas ces films).

## Game Boy / Game Boy Color (campagne autonome)

| Émulateur source | Format | Profil appliqué | Statut | Preuve |
|---|---|---|---|---|
| BizHawk 2.3 / 2.3.0 (Gambatte, CGB) | BK2 | horloge 2.3.x, boot ROM CGB | **pris en charge** | Zelda DX V1.0 : 4017M (100 045 trames) et 4084M (18 698 trames) atteignent la fin du jeu |
| BizHawk 2.3.2 (Gambatte, CGB) | BK2 | idem + `RTCDivisorOffset` du film | **partiel** | Pokémon Crystal : registres alignés sur Gambatte jusqu'à la trame 9 982 (2026-11-23), fin non atteinte |
| BizHawk 2.4.1 (Gambatte, CGB) | BK2 | Gambatte, boot ROM CGB | **non vérifié** | 4929M porte sur la version japonaise de Zelda DX : pas encore rejoué ici |
| BizHawk 2.9.1 (Gambatte, CGB) | BK2 | Gambatte, boot ROM CGB | **non vérifié** | 8500S porte sur la version japonaise de Zelda DX : pas encore rejoué ici |
| BizHawk GBHawk | BK2 | latch manette, phase DIV | **non vérifié** | comparaison de trace non concluante (docs/GB.fr.md, 0.182.1 et 0.218.0) |
| VBA-rr, VBA-rr v22, VBA-rr v23.6 svn480 | VBM | VBA V7 ou V8 | **désactivé** | Zelda DX : désynchronisation (599M, 1462M, 2628M). Voir la note ci-dessous |

Note VBA-rr : désactivation décidée le 2026-11-25 (`CAMP_SKIP_EMULATOR` dans `src/explore/campaign.h`). Un cas
contraire est connu et reste à reprendre à la réactivation : 2076M, sur la ROM DMG « Link's Awakening » V1.0, profil
VBA V7, déroule ses 13 714 trames jusqu'à la fin du jeu.

## Autres plateformes (rejeu hors campagne autonome, état des dernières mesures)

| Plateforme | Émulateur source | Statut | Preuve |
|---|---|---|---|
| Mega Drive | Gens-rr (GMV) | **partiel** | Sonic 1 et Streets of Rage : alignés sur de longues portions, ruptures connues (Sonic 1843S trame 37 318 ; SoR 1845S après la V-int 20 407) |
| PC Engine | BizHawk PCEHawk | **partiel** | Bonk III 4039M, 68 420 trames sans divergence soutenue mesurée |
| PC Engine | BizHawk 2.8 TurboNyma | **désynchronisé** | Shinobi (J) : le film ne se rejoue sur aucun émulateur actuel essayé |
