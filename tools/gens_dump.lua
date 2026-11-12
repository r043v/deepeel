-- dump the 64 KiB work RAM after each emulated frame of a replayed GMV movie
local out = io.open("Z:\\run\\media\\m\\Windows\\deepeel-data\\gens\\wram.bin", "wb")
local n = 6000
movie.play("Z:\\run\\media\\m\\Windows\\deepeel-data\\dsy\\s6k.gmv")
for f = 1, n do
    gens.emulateframe()
    local t = memory.readbyterange(0xFF0000, 0x10000)
    local s = {}
    for i = 1, 0x10000 do s[i] = string.char(t[i]) end
    out:write(table.concat(s))
    out:flush()
end
out:close()
os.exit(0)
