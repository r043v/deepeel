// bizhawk_dsy: replay a movie (masks.bin) through any supported BizHawk core and write the per-frame state in the
// DSY1 exchange format (src/core/desync.h), so `deepeel desync` compares BizHawk with a deepeel core.
// The memory blocks are the core's own domains (RAM-like ones, never ROM / System Bus), so the same tool serves
// every system; a system is one line in Cores[].
//
// usage: bizhawk_dsy SYSTEM ROM masks.bin out.dsy [FRAMES]       SYSTEM: gen | sms | nes | pce
//   masks.bin: one byte per frame, the lr_dsy layout (bit0 Up 1 Down 2 Left 3 Right 4 B 5 A 6 Select 7 Start);
//   each bit drives the core button whose name ends with the same word (first controller port).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

using BizHawk.Common;
using BizHawk.Client.Common;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores;
using BizHawk.Emulation.Cores.Consoles.Sega.gpgx;
using BizHawk.Emulation.Cores.Consoles.NEC.PCE;
using BizHawk.Emulation.Cores.Nintendo.NES;
using BizHawk.Emulation.Cores.Waterbox;
using SMSHawk = BizHawk.Emulation.Cores.Sega.MasterSystem.SMS;
using BizHawk.Tests.Testroms;

namespace BizhawkDsy
{
	internal static class Program
	{
		private static readonly string[] Words = { "Up", "Down", "Left", "Right", "B", "A", "Select", "Start" };

		// domain name -> DSY1 block name; anything not listed is not written (ROM, System Bus, CartRAM...)
		private static readonly Dictionary<string, string> Blocks = new()
		{
			{ "68K RAM", "wram" }, { "Main RAM", "wram" }, { "WRAM", "wram" }, { "RAM", "wram" },
			{ "Z80 RAM", "zram" }, { "Main Memory", "wram" }, { "Work Ram", "wram" }, { "CIRAM (nametables)", "vram" }, { "PALRAM", "cram" }, { "OAM", "oam" }, { "VRAM", "vram" }, { "CRAM", "cram" }, { "VSRAM", "vsrm" },
		};

		private static IEmulator Make(string system, byte[] rom, CoreComm coreComm, DummyFrontend.EmbeddedFirmwareProvider efp)
		{
			switch (system)
			{
				case "gen":
					return new GPGX(new CoreLoadParameters<GPGX.GPGXSettings, GPGX.GPGXSyncSettings>
					{
						Comm = coreComm, DeterministicEmulationRequested = true, Game = GameInfo.NullInstance,
						Roms = { new DummyRomAsset(rom, VSystemID.Raw.GEN) }, Settings = new(), SyncSettings = new(),
					});
				case "sms":
					return new SMSHawk(coreComm, GameInfo.NullInstance, rom, new(), new SMSHawk.SmsSyncSettings { UseBios = false });
				case "nes":
					BizHawk.Emulation.Common.Database.InitializeDatabase(Environment.GetEnvironmentVariable("BIZHAWK_GAMEDB") ?? (Environment.GetEnvironmentVariable("HOME") + "/dev/BizHawk/Assets/gamedb"), null, true);   // NesHawk waits on the game database
					BootGodDb.Initialize(Environment.GetEnvironmentVariable("BIZHAWK_GAMEDB") ?? (Environment.GetEnvironmentVariable("HOME") + "/dev/BizHawk/Assets/gamedb"));   // NesHawk, the BizHawk NES core
					efp.EmbedPathMap[new FirmwareID("NES", "Bios_FDS")] = "none";   // no FDS BIOS: the provider answers "absent" instead of throwing
					return new NES(coreComm, GameInfo.NullInstance, rom, new(), new NES.NESSyncSettings   // work RAM at zero like deepeel's machines; BIZHAWK_NES_RAM=power keeps the core's FCEUX power-up pattern
					{ InitialWRamStatePattern = Environment.GetEnvironmentVariable("BIZHAWK_NES_RAM") == "power" ? new byte[0] : new byte[] { 0 } });
				case "pce":   // TurboNyma (Mednafen PCE, waterbox), the BizHawk PC Engine core
					return new TurboNyma(new CoreLoadParameters<NymaCore.NymaSettings, NymaCore.NymaSyncSettings>
					{
						Comm = coreComm, DeterministicEmulationRequested = true, Game = GameInfo.NullInstance,
						Roms = { new DummyRomAsset(rom, VSystemID.Raw.PCE) }, Settings = new(), SyncSettings = new(),
					});
				default:
					Console.Error.WriteLine("unknown system " + system + " (gen | sms | nes | pce)");
					Environment.Exit(2);
					return null;
			}
		}

		private static void Main(string[] args)
		{
			if (args.Length < 4) { Console.Error.WriteLine("usage: bizhawk_dsy SYSTEM ROM masks.bin out.dsy [FRAMES]"); Environment.Exit(2); }
			var rom = File.ReadAllBytes(args[1]);
			var masks = File.ReadAllBytes(args[2]);
			int frames = args.Length > 4 ? Math.Min(int.Parse(args[4]), masks.Length) : masks.Length;
			using var fe = new DummyFrontend((efp, config, coreComm) => (Make(args[0], rom, coreComm, efp), 0));
			var def = fe.Core.ControllerDefinition;
			var names = new string[8];
			for (int i = 0; i < 8; i++)   // PC Engine names: II = B, I = A, Run = Start
				names[i] = def.BoolButtons.FirstOrDefault(b => b.StartsWith("P1 ") && (b.EndsWith(" " + Words[i]) || (args[0] == "pce" && b.EndsWith(" " + (i == 4 ? "II" : i == 5 ? "I" : i == 7 ? "Run" : "\0")))));
			Console.Error.WriteLine("buttons: " + string.Join(", ", names.Select((n, i) => Words[i] + "=" + (n ?? "-"))));
			var controller = new SimpleController(def);
			var domains = fe.CoreAsMemDomains;
			var picked = new List<(string block, MemoryDomain dom)>();
			var used = new HashSet<string>();
			foreach (var d in domains)
				if (Blocks.TryGetValue(d.Name, out var blk) && used.Add(blk)) picked.Add((blk, d));
			Console.Error.WriteLine("blocks: " + string.Join(", ", picked.Select(p => p.block + "<-" + p.dom.Name + "(" + p.dom.Size + ")")));
			// BIZHAWK_DSY_RANGE=lo-hi (hex, inclusive): only that window of every block is read, the rest stays zero
			long lo = 0, hi = long.MaxValue;
			var rg = Environment.GetEnvironmentVariable("BIZHAWK_DSY_RANGE");
			if (rg != null) { var p = rg.Split('-'); lo = Convert.ToInt64(p[0], 16); hi = Convert.ToInt64(p[1], 16) + 1; }
			using var w = new BinaryWriter(File.Create(args[3]));
			w.Write(Encoding.ASCII.GetBytes("DSY1"));
			for (int f = 0; f < frames; f++)
			{
				for (int i = 0; i < 8; i++) if (names[i] != null) controller[names[i]] = (masks[f] & (1 << i)) != 0;
				fe.Core.FrameAdvance(controller, true, false);
				w.Write((uint)(f + 1)); w.Write((ushort)picked.Count);
				foreach (var (blk, dom) in picked)
				{
					var bn = Encoding.ASCII.GetBytes(blk);
					w.Write((byte)bn.Length); w.Write(bn); w.Write((uint)dom.Size);
					var buf = new byte[dom.Size];
					if (lo == 0 && hi >= dom.Size) dom.BulkPeekByte(0L.RangeToExclusive(dom.Size), buf);
					else for (long a = lo; a < Math.Min(hi, dom.Size); a++) buf[a] = dom.PeekByte(a);   // peeks are slow on a waterbox core: a window keeps replays fast
					w.Write(buf);
				}
			}
		}
	}
}
