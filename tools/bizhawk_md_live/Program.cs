// bizhawk_md_live: replay a Mega Drive movie through BizHawk's GPGX core and publish each frame
// into the deepeel watch feed (src/explore/md_ref_feed.h layout), like tools/md_ref_live for ClownMDEmu.
//
// usage: bizhawk_md_live ROM masks.bin feedfile
//   masks.bin: one byte per frame, GMV order (bit0 Up, 1 Down, 2 Left, 3 Right, 4 B, 5 C, 6 A, 7 Start)
//   feedfile : the shared feed created by deepeel (DEEPEEL_MD_REF2_FEED)
using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Threading;

using BizHawk.Client.Common;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores;
using BizHawk.Emulation.Cores.Consoles.Sega.gpgx;
using BizHawk.Tests.Testroms;

namespace BizhawkMdLive
{
	internal static class Program
	{
		// feed layout: magic seq vframe total w h done leader unsync_x10, then the ARGB pixels
		private const int OffMagic = 0, OffSeq = 4, OffV = 8, OffTotal = 12, OffW = 16, OffH = 20, OffDone = 24, OffLeader = 28, OffUnsync = 32, OffPx = 36;
		private const int MaxPx = 426 * 262;

		private static void Main(string[] args)
		{
			if (args.Length < 3) { Console.Error.WriteLine("usage: bizhawk_md_live ROM masks.bin feedfile"); Environment.Exit(2); }
			var rom = File.ReadAllBytes(args[0]);
			var masks = File.ReadAllBytes(args[1]);
			using var mmf = MemoryMappedFile.CreateFromFile(args[2], FileMode.Open, null, 0);
			using var acc = mmf.CreateViewAccessor();
			using var fe = new DummyFrontend((efp, config, coreComm) =>
			{
				var core = new GPGX(new CoreLoadParameters<GPGX.GPGXSettings, GPGX.GPGXSyncSettings>
				{
					Comm = coreComm,
					DeterministicEmulationRequested = true,
					Game = GameInfo.NullInstance,
					Roms = { new DummyRomAsset(rom, VSystemID.Raw.GEN) },
					Settings = new(),
					SyncSettings = new(),
				});
				return (core, 0);
			});
			var controller = new SimpleController(fe.Core.ControllerDefinition);
			var vp = fe.Core.AsVideoProvider(); Console.Error.WriteLine("BUTTONS " + string.Join(",", fe.Core.ControllerDefinition.BoolButtons));
			string[] bits = { "P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 B", "P1 C", "P1 A", "P1 Start" };
			var clock = Stopwatch.StartNew();
			double waited = 0, lastMove = 0;
			uint lastLeader = 0;
			for (int f = 0; f < masks.Length; f++)
			{
				// replay sync: stay within two frames of the deepeel core; a leader that stopped for 2 s releases us
				for (;;)
				{
					double now = clock.Elapsed.TotalSeconds;
					uint leader = acc.ReadUInt32(OffLeader);
					if (leader != lastLeader) { lastLeader = leader; lastMove = now; }
					if (f <= leader + 2 || (lastLeader != 0 && now - lastMove > 2.0) || (lastLeader == 0 && now > 90.0)) break;
					Thread.Sleep(1); waited += 0.001;
				}
				for (int b = 0; b < 8; b++) controller[bits[b]] = (masks[f] & (1 << b)) != 0;
				fe.Core.FrameAdvance(controller, true, false);
				int w = vp.BufferWidth, h = vp.BufferHeight;
				var buf = vp.GetVideoBuffer();
				if (w > 0 && h > 0 && w * h <= MaxPx && buf.Length >= w * h)
				{
					uint seq = acc.ReadUInt32(OffSeq);
					acc.Write(OffSeq, seq + 1);
					unsafe { byte *bp = null; acc.SafeMemoryMappedViewHandle.AcquirePointer(ref bp); System.Runtime.InteropServices.Marshal.Copy(buf, 0, (IntPtr)(bp + acc.PointerOffset + OffPx), w * h); acc.SafeMemoryMappedViewHandle.ReleasePointer(); }
					acc.Write(OffW, (uint)w); acc.Write(OffH, (uint)h);
					acc.Write(OffV, (uint)(f + 1)); acc.Write(OffTotal, (uint)masks.Length);
					acc.Write(OffDone, f + 1 == masks.Length ? 1u : 0u);
					double busy = clock.Elapsed.TotalSeconds - waited;
					if (f > 0 && f % 30 == 0 && busy > 0.01) acc.Write(OffUnsync, (uint)(10.0 * f / busy));
					acc.Write(OffSeq, seq + 2);
				}
			}
		}
	}
}
