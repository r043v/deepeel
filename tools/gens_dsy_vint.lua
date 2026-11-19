-- Variant of the per-frame sampler: the sample is taken when the 68000 first executes the V-int vector ($78), so both machines
-- are compared at the same event (frame boundaries no longer depend on each emulator's own frame grid).
-- Gens-rr: replay a GMV and write one DSY1 frame per emulated frame ("wram" 64 KiB big-endian + "regs" D0-7,A0-7,PC as u32 LE),
-- readable by `deepeel desync`. Paths come from the environment-free constants below (edit or sed them).
local OUT, MOVIE, N = "Z:\\OUT.dsy", "Z:\\MOVIE.gmv", 3000
local chr = {}; for i = 0, 255 do chr[i] = string.char(i) end
local function u32(v) return chr[v % 256] .. chr[math.floor(v / 256) % 256] .. chr[math.floor(v / 65536) % 256] .. chr[math.floor(v / 16777216) % 256] end
local out = io.open(OUT, "wb"); out:write("DSY1")
movie.play(MOVIE)
local names = {}
for i = 0, 7 do names[#names + 1] = "main.d" .. i end
for i = 0, 7 do names[#names + 1] = "main.a" .. i end
names[#names + 1] = "main.pc"
local vec = 0
for i = 0, 3 do vec = vec * 256 + memory.readbyte(0x78 + i) end
local f = 0
local function sample()
    local t = memory.readbyterange(0xFF0000, 0x10000)
    local s = {}
    for i = 1, 0x10000 do s[i] = chr[t[i]] end
    local regs = {}
    for i, n in ipairs(names) do regs[i] = u32(memory.getregister(n) or 0) end
    out:write(u32(f), chr[2], chr[0])
    out:write(chr[4], "wram", u32(0x10000), table.concat(s))
    local r = table.concat(regs)
    out:write(chr[4], "regs", u32(#r), r)
    out:flush(); f = f + 1
end
memory.registerexec(vec, sample)
for k = 1, N do gens.emulateframe() end
out:close(); os.exit(0)
