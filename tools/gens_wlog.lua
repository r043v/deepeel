-- Gens-rr: log writes to $FFC0A8-AF between V-int sample 4686 and 4691 (V-int counted like v11.lua)
local OUT = io.open("Z:\\run\\media\\m\\Windows\\deepeel-data\\sor-sync\\gens_w.txt", "w")
movie.play("Z:\\run\\media\\m\\Windows\\deepeel-data\\tas\\sor\\1064M\\aka,mukki-streetsofrage.gmv")
local vec = 0
for i = 0, 3 do vec = vec * 256 + memory.readbyte(0x78 + i) end
local n = 0
memory.registerexec(vec, function() if n >= 4686 and n <= 4691 then OUT:write(string.format("V n=%d\n", n)); OUT:flush() end n = n + 1 end)
for a = 0xFFC0A8, 0xFFC0AF do
  memory.registerwrite(a, function()
    if n >= 4687 and n <= 4692 then
      OUT:write(string.format("W v=%d pc=%06x a=%06x val=%02x\n", n - 1, memory.getregister("main.pc") or 0, a, memory.readbyte(a))); OUT:flush()
    end
  end)
end
for k = 1, 5200 do gens.emulateframe() end
OUT:close(); os.exit(0)
