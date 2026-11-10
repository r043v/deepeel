// smshawk_dump: replay an SMS TAS movie through BizHawk's SMSHawk core and
// dump the same streams the deepeel GG/SMS bench produces (8 KiB work RAM per
// frame, same $C000-$DFFF window; optional PPM screens).  The output aligns
// frame-by-frame with DP_GG_DUMP/HASHLOG so m0boy, GPGX and SMSHawk compare
// three ways offline.
//
// usage: smshawk_dump ROM masks.bin frames outprefix [ppm frame...]
//   masks.bin: one byte per frame in the bench layout (bit0 Up, bit1 Down,
//   bit2 Left, bit3 Right, bit4 B1, bit5 B2, bit6 Pause/Start)
using System;
using System.IO;
using System.Linq;
using System.Text;

using BizHawk.Common;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores;
using BizHawk.Client.Common;                     // SimpleController
using BizHawk.Emulation.Cores.Sega.MasterSystem;
using SMSHawk = BizHawk.Emulation.Cores.Sega.MasterSystem.SMS;
using BizHawk.Tests.Testroms;

namespace SmshawkDump
{
	internal static class Program
	{
		private static void Main(string[] args)
		{
			Console.Error.WriteLine("stage: start");
			if (args.Length < 4)
			{
				Console.Error.WriteLine("usage: smshawk_dump ROM masks.bin frames outprefix [ppm frame...]");
				Environment.Exit(2);
			}
			var romPath = args[0];
			var masks = File.ReadAllBytes(args[1]);
			var frames = int.Parse(args[2]);
			var outPrefix = args[3];
			var ppmFrames = args.Skip(4).Select(int.Parse).ToHashSet();

			var rom = File.ReadAllBytes(romPath);
			var game = GameInfo.NullInstance; // no gamedb lookup: the DB load hangs under mono, the core only needs a name
			Console.Error.WriteLine("stage: init frontend");
			using var fe = new DummyFrontend((efp, config, coreComm) =>
			{
				// UseBios=false: the bench's m0boy instance and GPGX libretro host
				// both boot cartridge-only, the dumper must match that profile
				// SMSSHAWK_BIOS=1: boot with the BIOS ROM from Firmware/SMS/<region>.rom
				// (the bench's m0boy/GPGX instances boot cartridge-only, but the
				// BIOS-less boot diverges: the copyright scene countdown starts
				// ~575 frames late — see the Castle timer trace)
				var useBios = Environment.GetEnvironmentVariable("SMSSHAWK_BIOS") == "1";
				IEmulator core = new SMSHawk(coreComm, game, rom, new(), new SMSHawk.SmsSyncSettings { UseBios = useBios });
				return (core, 0);
			});

			Console.Error.WriteLine("stage: core ok");
			var controller = new SimpleController(fe.Core.ControllerDefinition);
			using var ram = File.Create($"{outPrefix}.hawk.bin");
			using var hash = File.Create($"{outPrefix}.hawk.hashlog");
			var rawPrefix = Environment.GetEnvironmentVariable("SMSSHAWK_RAW");
			BinaryWriter raw = null;
			if (rawPrefix != null) raw = new BinaryWriter(File.Create(rawPrefix));
			var bus = fe.CoreAsMemDomains!["System Bus"];
			var vp = fe.Core.AsVideoProvider();
			// bench watch: SMSSHAWK_WATCH=C31F (+ SMSSHAWK_WATCH_FROM/TO, 1-based
			// inclusive) logs the pc of every write to that work-RAM byte, via the
			// core's memory-callback system — the intra-frame counterpart of the
			// bench's DP_GG_TRACE_ADDR
			uint watchFrom = 1, watchTo = uint.MaxValue;
			uint? watchAddr = null;
			{   var w = Environment.GetEnvironmentVariable("SMSSHAWK_WATCH");
				// the address is absolute ($C31F): the core reports full-bus addresses
				if (w == "ALL") watchAddr = uint.MaxValue; // no address filter: every write
				else if (w != null) watchAddr = Convert.ToUInt32(w, 16);
				var wf = Environment.GetEnvironmentVariable("SMSSHAWK_WATCH_FROM");
				if (wf != null) watchFrom = uint.Parse(wf);
				var wt = Environment.GetEnvironmentVariable("SMSSHAWK_WATCH_TO");
				if (wt != null) watchTo = uint.Parse(wt);
			}
			int watchFrame = 0;
			if (watchAddr != null)
			{
				var hawkCore = (SMSHawk)fe.Core;
				var mcs = hawkCore.MemoryCallbacks;
				mcs.Add(new MemoryCallback("System Bus", MemoryCallbackType.Write, "bench-watch",
					(address, value, flags) =>
					{
						if (watchAddr != uint.MaxValue && address != watchAddr) return null;
						if (watchFrame >= watchFrom && watchFrame <= watchTo)
							Console.Error.WriteLine($"watch f={watchFrame} pc={hawkCore.Cpu.RegPC:x4} a={address & 0xFFFF:x4} v={value & 0xFF:x2}");
						return null;
					}, watchAddr == uint.MaxValue ? null : (uint?)watchAddr, null));
			}
			// bench trace: SMSSHAWK_LDAR=1 logs every executed LD A,R (ED 5F) with
			// the frame, pc and R at fetch — the game's RNG taps, the counterpart of
			// the bench's DP_GG_LDAR (R compared where the game reads it, not at
			// the frame edge where the two cores stop at different instruction phases)
			if (Environment.GetEnvironmentVariable("SMSSHAWK_LDAR") == "1")
			{
				var hawkCore = (SMSHawk)fe.Core;
				hawkCore.MemoryCallbacks.Add(new MemoryCallback("System Bus", MemoryCallbackType.Execute, "bench-ldar",
					(address, value, flags) =>
					{
						if (bus.PeekByte(address & 0xFFFF) == 0xED && bus.PeekByte((address + 1) & 0xFFFF) == 0x5F)
							Console.Error.WriteLine($"ldar f={watchFrame} pc={address & 0xFFFF:x4} r={hawkCore.Cpu.GetCpuFlagsAndRegisters()["R"].Value & 0x7F:x2}");
						return null;
					}, null, null));
			}
			Console.Error.WriteLine("stage: loop");
			for (int f = 0; f < frames; f++)
			{
				watchFrame = f + 1;
				var m = f < masks.Length ? masks[f] : (byte)0;
				// the SMSHawk definition prefixes per-player: "P1 Up", "P1 B1",
				// plus the system keys "Pause"/"Reset"
				controller["P1 Up"] = (m & 1) != 0;
				controller["P1 Down"] = (m & 2) != 0;
				controller["P1 Left"] = (m & 4) != 0;
				controller["P1 Right"] = (m & 8) != 0;
				controller["P1 B1"] = (m & 16) != 0;
				controller["P1 B2"] = (m & 32) != 0;
				controller["Pause"] = (m & 64) != 0;
				if (f == 0 || f % 500 == 0) Console.Error.WriteLine($"frame {f}");
				{   var rf = Environment.GetEnvironmentVariable("SMSSHAWK_REGTRACE");
					if (rf != null) { var p2 = rf.Split('-'); if (f + 1 >= int.Parse(p2[0]) && f + 1 <= int.Parse(p2[^1])) { var regs2 = ((SMSHawk)fe.Core).Cpu.GetCpuFlagsAndRegisters(); Console.Error.WriteLine($"regs f={f + 1} pc={regs2["PC"].Value:x4} r={regs2["R"].Value & 0x7F:x2} aff={regs2["AF"].Value:x4}"); } }
				}
				// DummyFrontend's constructor already advanced the core by one frame
				// (FrameAdvanceTo(0) runs its trailing FrameAdvance unconditionally,
				// empty controller): that frame IS movie frame 0. Advancing again here
				// played every input one frame late against a real BizHawk playback —
				// the whole reference was shifted, and R with it (constant offset at
				// the game's LD A,R taps). SMSSHAWK_PREFRAME=1 restores the old shift.
				if (f > 0 || Environment.GetEnvironmentVariable("SMSSHAWK_PREFRAME") == "1")
					fe.Core.FrameAdvance(controller, render: true, renderSound: false);
				else if (m != 0) Console.Error.WriteLine("warning: movie frame 0 carries input, lost to the frontend's constructor frame");
				var buf = new byte[0x2000];
				for (int a = 0; a < buf.Length; a++) buf[a] = bus.PeekByte(0xC000 + a);
				ram.Write(buf, 0, buf.Length);
				uint h = 2166136261;
				foreach (var b in buf) { h ^= b; h *= 16777619; }
				// screen hash over the provider buffer (FNV-1a over the XRGB ints)
				uint sh = 2166136261;
				var vb = vp.GetVideoBuffer();
				int n = vp.BufferWidth * vp.BufferHeight;
				for (int p = 0; p < n; p++) { sh ^= (uint)vb[p]; sh *= 16777619; }
				var line = Encoding.ASCII.GetBytes($"f {f + 1} hawk={sh:x8} ram={h:x8}\n"); hash.Write(line, 0, line.Length);
				if (raw != null)
				{
					var b2 = vp.GetVideoBuffer();
					int w = vp.BufferWidth, hgt = vp.BufferHeight;
					for (int y = 0; y < hgt; y++)
						for (int x = 0; x < w; x++)
						{
							var v = b2[y * w + x];
							int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, bl = v & 0xFF;
							raw.Write((ushort)((r >> 3) << 11 | (g >> 2) << 5 | bl >> 3));
						}
				}
				if (ppmFrames.Contains(f + 1)) DumpPpm($"{outPrefix}.hawk.{f + 1}.ppm", vp);
			}
			Console.Error.WriteLine($"smshawk_dump: {frames} frames -> {outPrefix}.hawk.bin");
		}

		private static void DumpPpm(string path, IVideoProvider vp)
		{
			using var fs = File.Create(path);
			var vb = vp.GetVideoBuffer();
			using var bw = new BinaryWriter(fs);
			// one writer only: the header goes out as bytes (a StreamWriter
			// wrapping the same stream races the BinaryWriter's buffer)
			foreach (var b in Encoding.ASCII.GetBytes($"P6\n{vp.BufferWidth} {vp.BufferHeight}\n255\n")) bw.Write(b);
			for (int p = 0; p < vp.BufferWidth * vp.BufferHeight; p++)
			{
				var v = vb[p];
				bw.Write((byte)((v >> 16) & 0xFF)); // XRGB ints in BizHawk providers
				bw.Write((byte)((v >> 8) & 0xFF));
				bw.Write((byte)(v & 0xFF));
			}
		}
	}
}
