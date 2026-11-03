# Campagne multi-plateforme sur fixtures générées

Observations de l'outil (machine concrète, RAM à zéro) sur les fixtures
construites par `tests/build_fixtures.py` ; **pas** des mesures de matériel,
**pas** des ROMs réelles. Généré par `tools/campaign_fixtures.py`.

| Plateforme | Fixture | Code retour | Pas explorés | Octets connus (%) | Durée (s) |
|---|---|---|---|---|---|
| Game Boy | `fix_apu.gb` | 0 | 13098 | 100.0 | 0.17 |
| Game Boy | `fix_bankdata.gb` | 0 | 1862 | 100.0 | 0.01 |
| Game Boy | `fix_branch.gb` | 0 | 3755 | 100.0 | 0.01 |
| Game Boy | `fix_closure.gb` | 0 | 1644 | 100.0 | 0.01 |
| Game Boy | `fix_copy1.gb` | 0 | 3660 | 100.0 | 0.01 |
| Game Boy | `fix_copyx.gb` | 0 | 33963 | 100.0 | 0.38 |
| Game Boy | `fix_dma.gb` | 0 | 9 | 100.0 | 0.01 |
| Game Boy | `fix_hdma.gb` | 0 | 26 | 100.0 | 0.01 |
| Game Boy | `fix_irq.gb` | 0 | 16494 | 100.0 | 0.01 |
| Game Boy | `fix_jumptable.gb` | 0 | 89 | 100.0 | 0.01 |
| Game Boy | `fix_keyirq.gb` | 0 | 32792 | 100.0 | 0.01 |
| Game Boy | `fix_loop.gb` | 0 | 696330 | 100.0 | 0.04 |
| Game Boy | `fix_mbc5.gb` | 0 | 28 | 100.0 | 0.01 |
| Game Boy | `fix_nsdata.gb` | 0 | 79999996 | 100.0 | 4.16 |
| Game Boy | `fix_nsecho.gb` | 0 | 8 | 100.0 | 0.01 |
| Game Boy | `fix_nsstack.gb` | 0 | 6 | 100.0 | 0.01 |
| Game Boy | `fix_p1poll.gb` | 0 | 3481683 | 100.0 | 0.19 |
| Game Boy | `fix_pal.gb` | 0 | 4106 | 100.0 | 0.15 |
| Game Boy | `fix_probes.gb` | 0 | 22 | 100.0 | 0.01 |
| Game Boy | `fix_unreach.gb` | 0 | 11 | 99.2 | 0.00 |
| Game Boy | `fix_vblank.gb` | 0 | 13376707 | 100.0 | 2.66 |
| Mega Drive | `fix_md_cpu_vram.md` | 0 | 207 | n/d | 0.00 |
| Mega Drive | `fix_md_cpu_vram_gap.md` | 0 | 216 | n/d | 0.00 |
| Mega Drive | `fix_md_cpu_vram_reg.md` | 0 | 212 | n/d | 0.00 |
| Mega Drive | `fix_md_dbcc.md` | 0 | 202 | n/d | 0.01 |
| Mega Drive | `fix_md_dma.md` | 0 | 71 | n/d | 0.00 |
| Mega Drive | `fix_md_dma_fault.md` | 0 | 6 | n/d | 0.00 |
| Mega Drive | `fix_md_fault.md` | 0 | 1 | n/d | 0.00 |
| Mega Drive | `fix_md_fault_reg.md` | 0 | 2 | n/d | 0.00 |
| Mega Drive | `fix_md_fork.md` | 0 | 134 | n/d | 0.00 |
| Mega Drive | `fix_md_fork_fault.md` | 0 | 69 | n/d | 0.00 |
| Mega Drive | `fix_md_loop.md` | 0 | 65 | n/d | 0.00 |
| Mega Drive | `fix_md_pad.md` | 0 | 645 | n/d | 0.00 |
| Mega Drive | `fix_md_pad_effect.md` | 0 | 853 | n/d | 0.00 |
| Mega Drive | `fix_md_reader.md` | 0 | 202 | n/d | 0.00 |
| Mega Drive | `fix_md_reader_overlap.md` | 0 | 205 | n/d | 0.01 |
| Mega Drive | `fix_md_revisit.md` | 0 | 338 | n/d | 0.00 |
| Mega Drive | `fix_md_sram_overlay.md` | 0 | 10000000 | n/d | 0.70 |
| Mega Drive | `fix_md_vint.md` | 0 | 133 | n/d | 0.00 |
| Mega Drive | `fix_md_vint_masked.md` | 0 | 66 | n/d | 0.00 |
| Mega Drive | `fix_md_watch.md` | 0 | 2 | n/d | 0.00 |
| Mega Drive | `fix_md_z80.md` | 0 | 424 | n/d | 0.01 |
| Master System | `fix_sega_mapper.sms` | 0 | n/d | 99.98 | 1.15 |

## Synthèse par plateforme

| Plateforme | Fixtures | Échecs | Pas (total) |
|---|---|---|---|
| Game Boy | 21 | 0 | 97666289 |
| Mega Drive | 21 | 0 | 10004053 |
| Master System | 1 | 0 | 0 |

`n/d` : valeur non exposée par le rapport texte de la plateforme.
NES et PC Engine n'ont pas de fixtures fichier : leurs ROMs sont construites
en mémoire par `test_nes` et `test_pce`, hors de cette campagne.
