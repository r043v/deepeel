# Replace only the line interrupt phase in deepeel's copy of the runtime.
# The normal profile returns the upstream value; TAS playback can select the
# SMSHawk phase without changing m0boy's hardware model.
file(READ "${INPUT}" source)
string(REGEX MATCHALL "GG_LINE_IRQ_AT" matches "${source}")
list(LENGTH matches count)
if(NOT count EQUAL 4)
  message(FATAL_ERROR "GG line interrupt phase anchors changed; audit the runtime")
endif()
string(REPLACE "GG_LINE_IRQ_AT" "gg_line_irq_phase()" source "${source}")
string(REPLACE "#include \"gg.h\"" "#include \"gg.h\"\nextern unsigned gg_line_irq_phase(void);" source "${source}")
file(WRITE "${OUTPUT}" "${source}")
