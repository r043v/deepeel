# The upstream runtime remains untouched. Refuse source drift at each bypass
# so a dependency update cannot silently remove cartridge/RAM observations.
file(READ "${INPUT}" source)
function(replace_once anchor replacement)
  string(FIND "${source}" "${anchor}" position)
  if(position LESS 0)
    message(FATAL_ERROR "MSX instrumentation anchor missing; audit the updated runtime")
  endif()
  string(LENGTH "${anchor}" length)
  math(EXPR after "${position}+${length}")
  string(SUBSTRING "${source}" ${after} -1 tail)
  string(FIND "${tail}" "${anchor}" duplicate)
  if(NOT duplicate LESS 0)
    message(FATAL_ERROR "MSX instrumentation anchor repeated; audit the updated runtime")
  endif()
  string(REPLACE "${anchor}" "${replacement}" source "${source}")
  set(source "${source}" PARENT_SCOPE)
endfunction()
replace_once([=[#else
    return machine.rom[offset];
#endif
}

static M0_TL int mapper_override]=] [=[#else
    return dp_msx_bios_rom_read(offset, a, machine.rom[offset]);
#endif
}

static M0_TL int mapper_override]=])
replace_once([=[if (slot == 3 && a >= 0x10000u - SG_MSX_RAM) return m->ram[a - (0x10000u - SG_MSX_RAM)];]=]
  [=[if (slot == 3 && a >= 0x10000u - SG_MSX_RAM) return dp_msx_bios_ram_read(a, m->ram[a - (0x10000u - SG_MSX_RAM)]);]=])
replace_once([=[if ((slot & 3) == 3 && a >= 0x10000u - SG_MSX_RAM) machine.ram[a - (0x10000u - SG_MSX_RAM)] = v;]=]
  [=[if ((slot & 3) == 3 && a >= 0x10000u - SG_MSX_RAM) {
        machine.ram[a - (0x10000u - SG_MSX_RAM)] = v;
        dp_msx_bios_ram_written(a, v);
    }]=])
# The physical port decoder must call the native VDP, not re-enter the
# instrumented CPU bus. Only BIOS-side helpers above this section are wrapped.
replace_once([=[/* ---- ports ---------------------------------------------------------------- */]=]
  [=[#undef msx_vdp_in
#undef msx_vdp_out
#undef msx_vdp_data
/* ---- ports ---------------------------------------------------------------- */]=])
# CHPUT/scroll take the line width from LINLEN in RAM, which a game may overwrite: text_scroll keeps a
# 40-byte line buffer, so a larger value smashed the stack (seen on Gradius 2).  Clamp it to the buffer.
replace_once([=[const unsigned stride = mode ? 32 : 40, columns = rd(LINLEN);]=]
  [=[const unsigned stride = mode ? 32 : 40, columns = rd(LINLEN) > 40 ? 40 : rd(LINLEN);]=])
file(WRITE "${OUTPUT}" "/* Generated MSX BIOS instrumentation; do not edit. */\n#include \"msx_hooks.h\"\n${source}")
