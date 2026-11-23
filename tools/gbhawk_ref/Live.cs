using System;
using System.Collections.Generic;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Threading;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores.Nintendo.GBHawk;

// GBHawk live reference tile for the deepeel watch window: `gbh --live ROM inputs.txt BIOS FEED [dmg]`.
// Same feed layout as src/explore/md_ref_feed.h (DpMdRefFeed); the core stays within two frames of the
// frame counter the window publishes (`leader`), exactly like tools/gb_ref_live.
static class Live {
    const int Magic = 0x52464D44, Max = 426 * 262, Size = 36 + Max * 4;
    public static int Run(string[] a) {
        var rom = File.ReadAllBytes(a[1]);
        var keys = new[] { "Up", "Down", "Left", "Right", "Start", "Select", "B", "A" };
        var frames = new List<string>();
        foreach (var l in File.ReadLines(a[2])) if (l.StartsWith("|") || l.Length == 9) frames.Add(l.StartsWith("|") ? l.Substring(1) : l);
        var bios = File.ReadAllBytes(a[3]);
        bool dmg = a.Length > 5 && a[5] == "dmg";
        var comm = new CoreComm(s => { }, (s, d) => { }, new Fw { Bios = bios }, CoreComm.CorePreferencesFlags.None);
        var ss = new GBHawk.GBSyncSettings { ConsoleMode = dmg ? GBHawk.GBSyncSettings.ConsoleModeType.GB : GBHawk.GBSyncSettings.ConsoleModeType.GBC, GBACGB = false, Use_SRAM = false };
        var gb = new GBHawk(comm, GameInfo.NullInstance, rom, new GBHawk.GBSettings(), ss);
        var pad = new Pad { Definition = gb.ControllerDefinition };
        using var fs = new FileStream(a[4], FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        fs.SetLength(Size);
        using var mm = MemoryMappedFile.CreateFromFile(fs, null, Size, MemoryMappedFileAccess.ReadWrite, HandleInheritability.None, true);
        using var v = mm.CreateViewAccessor(0, Size);
        for (long i = 0; i < Size; i += 4) v.Write(i, 0);
        v.Write(0, Magic);
        var px = new int[160 * 144];
        var t0 = DateTime.UtcNow; double lastMove = 0, waited = 0; uint lastLeader = 0;
        double release = double.Parse(Environment.GetEnvironmentVariable("GB_REF_RELEASE_S") ?? "2");
        for (int f = 0; f < frames.Count; f++) {
            for (;;) {
                double now = (DateTime.UtcNow - t0).TotalSeconds;
                uint leader = v.ReadUInt32(28);
                if (leader != lastLeader) { lastLeader = leader; lastMove = now; }
                if (f <= leader + 2 || (lastLeader != 0 && now - lastMove > release) || (lastLeader == 0 && now > release)) break;
                Thread.Sleep(0); Thread.SpinWait(2000); waited += 0;
            }
            pad.Down.Clear();
            for (int k = 0; k < 8; k++) if (frames[f][k] != '.') pad.Down.Add("P1 " + keys[k]);
            gb.FrameAdvance(pad, true, false);
            var vb = gb.GetVideoBuffer();
            for (int i = 0; i < px.Length && i < vb.Length; i++) px[i] = vb[i];
            v.Write(4, v.ReadUInt32(4) + 1);                   // seq odd
            v.WriteArray(36, px, 0, px.Length);
            v.Write(16, 160); v.Write(20, 144);
            v.Write(8, f + 1); v.Write(12, frames.Count); v.Write(24, f + 1 == frames.Count ? 1 : 0);
            v.Write(4, v.ReadUInt32(4) + 1);                   // seq even
        }
        return 0;
    }
}
