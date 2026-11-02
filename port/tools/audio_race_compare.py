"""The same race's sound on the PS2 (PCSX2) and in the port, compared by level and by voice
(finding 133). One game at a time.

    python port/tools/audio_race_compare.py build-pcsx2 --src PCSX2_TREE
    python port/tools/audio_race_compare.py retail aube-ps2 --track 15
    python port/tools/audio_race_compare.py port aube-new --track 15 --exe build/host64-exe/dhd64.exe
    python port/tools/audio_race_compare.py compare aube-ps2 aube-old aube-new --bank AUBE_S.BNK

build-pcsx2 rebuilds the comparison adapter (prepare_pcsx2_compare.py's tree, PCSX2 v2.8.2, NAV and
race hooks) as bin/pcsx2-dhd-navaudio.exe with an SPU2 capture hook in place of SPU2/spu2.cpp,
through MSBuild metadata (finding 022 round 3): wav/spu2-mix.wav (the raw mix before PCSX2's DC
filter), wav/voices.csv (every 800 samples each sounding voice: SPU address, pitch, ENVX, ADSR,
volumes, VMIX bits) and wav/core.csv (each core's MVOL, EVOL, input volumes, effect enable, MMIX).
The port side writes the same through DHD_SND_WAV and DHD_SND_VOICE_TRACE.

Both sides: race_compare.py's NAV with the track chooser moved right --track times (15 Mt.
Liddellroch FR, 0 Mt. Midoule FR; finding 063's order with every track unlocked), every track
unlocked (port DHD_UNLOCK_ALL=1; PCSX2 a pnach with the retail unlock code's track mask), race seed
81257, Cross held from 2 s after GO, stopped --stop race-clock units (60 a second) after GO.
compare aligns windows on each side's own countdown/GO fields and prints RMS dBFS, band shares
and the summed voice power (gain^2) by sound family (E_ ambient, B_ riders), PCSX2 voices named by
their SPU address minus the bank's upload address. Positions and random grain choices differ
between the sides: levels and spectra over the same stretch, not samples.

Disc: DHD_TEST_ISO (or --iso); BIOS: DHD_PCSX2_BIOS or pcsx2_retail.bios_file's default. Output:
build/audio/<label>/ (game audio: never commit it)."""
import argparse
import collections
import csv
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_pcsx2 as compare  # noqa: E402
import owned_process  # noqa: E402
import pcsx2_retail as retail  # noqa: E402
import race_compare as rc  # noqa: E402
import state_trace_run as st  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build' / 'audio'
EXE_NAME = 'pcsx2-dhd-navaudio'
PAD = '2:1200:cross'
UNLOCK_PNACH = 'gametitle=Downhill Domination\npatch=1,EE,204346B4,extended,035FFFFF\n'  # D_004225A0 + 0x12114
MSBUILD = Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'

HOOK = r'''
static FILE *dhd_raw, *dhd_voices, *dhd_spu_cores;
static u32 dhd_audio_samples;
static void DhdWaveHeader(FILE* file)
{
    if (!file) return;
    unsigned char h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' '};
    auto put = [&](int at, u32 value, int n) { for (int i = 0; i < n; i++) h[at + i] = static_cast<unsigned char>(value >> (8 * i)); };
    put(4, dhd_audio_samples * 4 + 36, 4); put(16, 16, 4); put(20, 1, 2); put(22, 2, 2);
    put(24, 48000, 4); put(28, 192000, 4); put(32, 4, 2); put(34, 16, 2);
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a'; put(40, dhd_audio_samples * 4, 4);
    std::fseek(file, 0, SEEK_SET); std::fwrite(h, 1, 44, file); std::fseek(file, 0, SEEK_END); std::fflush(file);
}
static void DhdAudioClose()
{
    DhdWaveHeader(dhd_raw);
    if (dhd_raw) std::fclose(dhd_raw);
    if (dhd_voices) std::fclose(dhd_voices);
    if (dhd_spu_cores) std::fclose(dhd_spu_cores);
    dhd_raw = dhd_voices = dhd_spu_cores = nullptr; dhd_audio_samples = 0;
}
static void DhdAudioCapture(StereoOut32 out)
{
    if (!dhd_raw) {
        const char* directory = std::getenv("DHD_AUDIO_REFERENCE");
        if (!directory) return;
        const std::string prefix = std::string(directory) + "\\";
        dhd_raw = std::fopen((prefix + "spu2-mix.wav").c_str(), "wb+");
        dhd_voices = std::fopen((prefix + "voices.csv").c_str(), "wb");
        dhd_spu_cores = std::fopen((prefix + "core.csv").c_str(), "wb");
        if (!dhd_raw || !dhd_voices || !dhd_spu_cores) { std::fprintf(stderr, "DHD_AUDIO_OPEN_ERROR\n"); std::exit(30); }
        DhdWaveHeader(dhd_raw);
        std::fprintf(dhd_voices, "sample,field,core,voice,ssa,lsa,pitch,envx,phase,adsr1,adsr2,vol_l,vol_r,reg_l,reg_r,dry_l,dry_r,wet_l,wet_r\n");
        std::fprintf(dhd_spu_cores, "sample,field,core,mvol_l,mvol_r,evol_l,evol_r,avol_l,avol_r,bvol_l,bvol_r,fx_enable,attr,mmix,esa,eea\n");
    }
    const s16 raw[2] = {static_cast<s16>(clamp_mix(out.Left)), static_cast<s16>(clamp_mix(out.Right))};
    std::fwrite(raw, 4, 1, dhd_raw);
    if (dhd_audio_samples % 800 == 0) {
        for (int core = 0; core < 2; core++) {
            const auto& c = Cores[core];
            std::fprintf(dhd_spu_cores, "%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u\n", dhd_audio_samples, g_FrameCount, core,
                c.MasterVol.Left.Value, c.MasterVol.Right.Value, c.FxVol.Left, c.FxVol.Right, c.InpVol.Left, c.InpVol.Right,
                c.ExtVol.Left, c.ExtVol.Right, c.FxEnable ? 1 : 0, (unsigned)c.Regs.ATTR, (unsigned)c.Regs.MMIX,
                (unsigned)c.EffectsStartA, (unsigned)c.EffectsEndA);
            for (int v = 0; v < 24; v++) {
                const auto& vc = c.Voices[v];
                if (vc.ADSR.Phase == 0 || vc.ADSR.Value == 0) continue;
                std::fprintf(dhd_voices, "%u,%u,%d,%d,%u,%u,%u,%d,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u\n", dhd_audio_samples, g_FrameCount,
                    core, v, vc.StartA, vc.LoopStartA, vc.Pitch, vc.ADSR.Value, vc.ADSR.Phase, vc.ADSR.regADSR1, vc.ADSR.regADSR2,
                    vc.Volume.Left.Value, vc.Volume.Right.Value, (unsigned)vc.Volume.Left.Reg_VOL, (unsigned)vc.Volume.Right.Reg_VOL,
                    (c.Regs.VMIXL >> v) & 1, (c.Regs.VMIXR >> v) & 1, (c.Regs.VMIXEL >> v) & 1, (c.Regs.VMIXER >> v) & 1);
            }
        }
    }
    dhd_audio_samples++;
    if (dhd_audio_samples % 48000 == 0) { DhdWaveHeader(dhd_raw); std::fflush(dhd_voices); std::fflush(dhd_spu_cores); }
}
'''


def build_pcsx2(src):
    """The adapter with the capture hook, into SRC/bin. The upstream sources are not edited."""
    private = OUT / 'pcsx2-hook'
    private.mkdir(parents=True, exist_ok=True)
    unit = (src / 'pcsx2/SPU2/spu2.cpp').read_text(encoding='utf-8')
    for anchor in ('#include "common/Error.h"', 'u64 lClocks = 0;', '\tDCFilter(conv);', 'void SPU2::Close()\n{'):
        if anchor not in unit:
            sys.exit(f'spu2.cpp has no {anchor!r}: not the PCSX2 v2.8.2 tree the hook was written for')
    unit = unit.replace('#include "common/Error.h"', '#include "common/Error.h"\n#include "Counters.h"\n'
                        '#include <cstdlib>\n#include <cstdio>\n#include <string>', 1)
    unit = unit.replace('u64 lClocks = 0;', HOOK + '\nu64 lClocks = 0;', 1)
    unit = unit.replace('\tDCFilter(conv);', '\tDhdAudioCapture(out);\n\tDCFilter(conv);', 1)
    unit = unit.replace('void SPU2::Close()\n{', 'void SPU2::Close()\n{\n\tDhdAudioClose();', 1)
    target = private / 'spu2-audio.cpp'
    target.write_text(unit, encoding='utf-8', newline='\n')
    props = private / 'audio.targets'
    props.write_text(f'''<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemDefinitionGroup Condition="'$(MSBuildProjectName)'=='pcsx2'">
<ClCompile><ProgramDataBaseFileName>{private / 'audio.pdb'}</ProgramDataBaseFileName>
<AdditionalOptions>/FS %(AdditionalOptions)</AdditionalOptions></ClCompile>
</ItemDefinitionGroup>
<Target Name="DhdAudioSource" BeforeTargets="ClCompile" Condition="'$(MSBuildProjectName)'=='pcsx2'">
<ItemGroup><ClCompile Remove="SPU2\\spu2.cpp" />
<ClCompile Include="{target}"><ObjectFileName>$(IntDir)spu2-audio.obj</ObjectFileName><PrecompiledHeader>NotUsing</PrecompiledHeader><AdditionalIncludeDirectories>{src / 'pcsx2'};{src / 'pcsx2' / 'SPU2'};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories></ClCompile></ItemGroup>
</Target></Project>''', encoding='utf-8')
    subprocess.run([str(MSBUILD), str(src / 'PCSX2_qt.sln'), '/t:pcsx2-qt', '/m:2', '/p:CL_MPCount=2',
                    '/p:Configuration=Release AVX2', '/p:Platform=x64', f'/p:EXEString={EXE_NAME}',
                    '/p:PlatformToolset=v145', '/p:DefaultPlatformToolset=v145',
                    '/p:ForceImportBeforeCppTargets=' + str(props), '/v:minimal', '/nologo'], check=True)
    print('built', src / 'bin' / (EXE_NAME + '.exe'))


def nav(track):
    prefix, suffix = st.RACE_NAV.split('18@4C7F38=1:cross', 1)
    return prefix + '18@4C7F38=1+240:right,' * track + '18@4C7F38=1+240:cross' + suffix


def run(side, a):
    d = OUT / a.label
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    base = {k: v for k, v in os.environ.items() if not k.startswith(('DHD_', 'ROUND_'))}
    if side == 'retail':
        emulator = Path(a.pcsx2).resolve()
        if not emulator.is_file():
            sys.exit(f'no {emulator}: run build-pcsx2 first, or give --pcsx2')
        runtime = d / 'pcsx2-runtime'
        runtime.mkdir()
        for item in emulator.parent.iterdir():
            if item.suffix in ('.dll', '.conf') or item == emulator:
                shutil.copyfile(item, runtime / item.name)
            elif item.is_dir() and item.name in ('resources', 'QtPlugins', 'D3D12', 'translations'):
                shutil.copytree(item, runtime / item.name)
        data = retail.config(d, retail.bios_file(os.environ.get('DHD_PCSX2_BIOS')), 13)
        ini = data / 'PCSX2/inis/PCSX2.ini'
        ini.write_text(ini.read_text().replace('EnableCheats = false', 'EnableCheats = true'))
        (data / 'PCSX2/cheats').mkdir(parents=True, exist_ok=True)
        (data / 'PCSX2/cheats/SCUS-97177_5AE01D98.pnach').write_text(UNLOCK_PNACH)
        compare.write_movie(d / 'navigation.p2m2', compare.parse_pad(rc.NAVIGATION), compare.field_at(620))
        compare.write_movie(d / 'race.p2m2', rc.clock_events(PAD), a.stop + 600)
        (d / 'wav').mkdir()
        env = dict(base, DHD_COMPARE_NAV=nav(a.track), DHD_COMPARE_RACE_SEED=str(rc.RETAIL_RACE_SEED),
                   DHD_COMPARE_MOVIE=str(d / 'navigation.p2m2'), DHD_COMPARE_RACE='1',
                   DHD_COMPARE_RACE_MOVIE=str(d / 'race.p2m2'), DHD_COMPARE_RACE_STOP=str(a.stop),
                   DHD_AUDIO_REFERENCE=str(d / 'wav'))
        cmd = [str(runtime / emulator.name), '-datapath', str(data), '-batch', '-nogui', '-fastboot', '-unlimited',
               '-logfile', str(d / 'pcsx2.log'), '--', a.iso]
        log = d / 'stdout.log'
    else:
        env = dict(base, DHD_ISO=a.iso, DHD_HEADLESS='1', DHD_FRAME_CLOCK='1', DHD_LOG='1', DHD_SKIP_MOVIES='1',
                   DHD_MEMCARD='0', DHD_SETTINGS='none', DHD_SND_OFF='1', DHD_RENDER='native', DHD_UNLOCK_ALL='1',
                   DHD_NAV=nav(a.track), DHD_RACE_SYNC='1', DHD_RACE_PAD=PAD, DHD_RACE_SEED=str(rc.RETAIL_RACE_SEED),
                   DHD_RACE_CAPTURE=f'g+{a.stop}', DHD_SCREENSHOT=str(d / 'shot'), DHD_SND_WAV=str(d / 'mix.wav'),
                   DHD_SND_VOICE_TRACE=str(d / 'voices.csv'), DHD_SND_GAIN_TRACE=str(d / 'gain.csv'),
                   DHD_SND_TRACE='1')
        cmd = [str(Path(a.exe).resolve())]
        log = d / 'port.log'
    start = time.monotonic()
    with open(log, 'wb') as f, owned_process.start(cmd, cwd=str(d), env=env, stdout=f, stderr=subprocess.STDOUT) as p:
        while p.poll() is None:
            if side == 'port' and re.search(rf'compare: race moment=g\+{a.stop}', log.read_text(errors='replace')):
                time.sleep(2)  # the stop moment's own field is rendered
                p.stop()
                break
            if time.monotonic() - start > a.timeout:
                p.stop()
                sys.exit(f'{side}: timeout; see {log}')
            time.sleep(1)
    print(f'{side} done in {time.monotonic() - start:.0f} s: {d}')


# ---- the comparison ----------------------------------------------------------------------------

def iso_file(iso, wanted):
    """A file's bytes from the ISO 9660 image, by its path (e.g. SND/AUBE_S.BNK)."""
    with open(iso, 'rb') as f:
        def sectors(lsn, count=1):
            f.seek(lsn * 2048)
            return f.read(2048 * count)

        def find(lsn, size, parts):
            data = sectors(lsn, (size + 2047) // 2048)
            at = 0
            while at < len(data):
                length = data[at]
                if not length:
                    at = (at // 2048 + 1) * 2048
                    continue
                extent, extent_size = struct.unpack_from('<I', data, at + 2)[0], struct.unpack_from('<I', data, at + 10)[0]
                name = data[at + 33:at + 33 + data[at + 32]].decode('latin1').split(';')[0]
                if name.upper() == parts[0].upper():
                    if len(parts) == 1:
                        f.seek(extent * 2048)
                        return f.read(extent_size)
                    return find(extent, extent_size, parts[1:])
                at += length
            sys.exit(f'{wanted} not on the disc')
        root = sectors(16)[156:156 + 34]
        return find(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], wanted.split('/'))


def bank_tones(data):
    """{tone sample offset: {sound names}} of a version-1 SBlk bank (sound.c snd_bank_parse's layout)."""
    rd32 = lambda o: struct.unpack_from('<I', data, o)[0]
    base, names = 0x18, {}
    count, first_sound, first_grain = struct.unpack_from('<H', data, base + 0x16)[0], rd32(base + 0x1C), rd32(base + 0x20)
    table_at = rd32(base + 0x34)
    table = rd32(base + table_at + 8) + table_at
    for bucket in range(32):
        index = struct.unpack_from('<h', data, base + table_at + 0x18 + bucket * 2)[0]
        at = table + index * 20
        while index >= 0 and rd32(base + at):
            names[struct.unpack_from('<h', data, base + at + 16)[0]] = data[base + at:base + at + 16].split(b'\0')[0].decode('latin1')
            at += 20
    tones = collections.defaultdict(set)
    for i in range(count):
        record = base + first_sound + i * 12
        for k in range(data[record + 4]):
            grain = base + first_grain + rd32(record + 8) + k * 40
            if rd32(grain) in (1, 9):
                tones[rd32(grain + 8 + 16)].add(names.get(i, ''))
    return tones


def load(label):
    d = OUT / label
    if (d / 'stdout.log').exists():  # PCSX2
        anchor = dict(re.findall(r'DHD_COMPARE_RACE anchor=([cg]) field=(\d+)', (d / 'stdout.log').read_text(errors='replace')))
        rows = list(csv.DictReader(open(d / 'wav/core.csv')))[::2]
        fields, samples = np.array([int(r['field']) for r in rows]), np.array([int(r['sample']) for r in rows])
        field_sample = lambda field: int(np.interp(field, fields, samples))
        path, voices = d / 'wav/spu2-mix.wav', d / 'wav/voices.csv'
    else:
        anchor = dict(re.findall(r'compare: race anchor=([cg]) field=(\d+)', (d / 'port.log').read_text(errors='replace')))
        field_sample = lambda field: field * 4004 // 5  # sound.c: 4004/5 samples per field
        path, voices = d / 'mix.wav', d / 'voices.csv'
    with wave.open(str(path)) as w:
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2).astype(np.float64)
    return dict(label=label, retail=(d / 'stdout.log').exists(), anchor={k: int(v) for k, v in anchor.items()},
                field_sample=field_sample, pcm=pcm, voices=voices)


def levels(side):
    c, g, f2s = side['anchor']['c'], side['anchor']['g'], side['field_sample']
    windows = {'20 s before the countdown': (c - 1199, c), 'countdown': (c, g)}
    for lo, hi in ((0, 10), (10, 30), (30, 60), (0, 60)):
        windows[f'race {lo}-{hi} s'] = (g + round(lo * 59.94), g + round(hi * 59.94))
    out = {}
    for name, (a, b) in windows.items():
        seg = side['pcm'][f2s(a):f2s(b)]
        if len(seg) < 4800:
            continue
        power = np.abs(np.fft.rfft(seg.mean(axis=1) * np.hanning(len(seg)))) ** 2
        freqs = np.fft.rfftfreq(len(seg), 1 / 48000)
        bands = [round(float(100 * power[(freqs >= lo) & (freqs < hi)].sum() / power.sum()), 1)
                 for lo, hi in ((0, 200), (200, 1000), (1000, 4000), (4000, 24000))]
        out[name] = dict(rms_dbfs=round(20 * math.log10(max(np.sqrt((seg ** 2).mean()), 1e-9) / 32768), 2), bands=bands)
    return out


def families(side, tones, base, seconds=60):
    """Summed voice gain^2 and sounding rows by family over the first `seconds` after GO."""
    power, rows = collections.Counter(), collections.Counter()
    g = side['anchor']['g']
    for r in csv.DictReader(open(side['voices'])):
        t = ((int(r['field']) if side['retail'] else int(r['sample']) * 5 / 4004) - g) / 59.94
        if not 0 <= t < seconds:
            continue
        if side['retail']:
            off = (int(r['ssa']) - base) * 2
            name = sorted(tones[off])[0] if off in tones else ''
        else:
            name = r['sound']
        family = name[:2] if name[:2] in ('E_', 'B_', 'R_') else 'other'
        gain = max(abs(int(r['vol_l'])), abs(int(r['vol_r']))) * int(r['envx']) / 32767
        power[family] += gain * gain
        rows[family] += 1
    return power, rows


def spu_base(side, tones):
    """The bank's SPU upload address (words): the most common voice address minus a tone offset."""
    g = side['anchor']['g']
    addresses = collections.Counter(int(r['ssa']) for r in csv.DictReader(open(side['voices'])) if int(r['field']) >= g)
    votes = collections.Counter()
    for address in addresses:
        for off in tones:
            votes[address - off // 2] += 1
    return votes.most_common(1)[0][0]


def compare_runs(a):
    sides = [load(label) for label in a.labels]
    if not sides[0]['retail']:
        sys.exit('the first label must be a PCSX2 run')
    tones = bank_tones(iso_file(a.iso, 'SND/' + a.bank))
    base = spu_base(sides[0], tones)
    result = {s['label']: dict(anchor=s['anchor'], levels=levels(s)) for s in sides}
    ref_power, ref_rows = families(sides[0], tones, base)
    for s in sides:
        power, rows = families(s, tones, base)
        result[s['label']]['families'] = {k: dict(rows=rows[k], power_db_vs_pcsx2=round(10 * math.log10(power[k] / ref_power[k]), 1)
                                                  if power[k] and ref_power[k] else None) for k in ('E_', 'B_')}
    names = [s['label'] for s in sides]
    print(f'{a.bank}: SPU base {base} words; RMS dBFS, windows on each side\'s own countdown (c) and GO (g)')
    print(' ' * 26 + ''.join(n[:14].rjust(15) for n in names))
    for window in result[names[0]]['levels']:
        print(window.ljust(26) + ''.join(str(result[n]['levels'].get(window, {}).get('rms_dbfs', '-')).rjust(15) for n in names))
    print('race 0-60 s bands 0-200/200-1000/1000-4000/4000+ Hz %: ' +
          ' | '.join(f"{n} {result[n]['levels'].get('race 0-60 s', {}).get('bands')}" for n in names))
    for family in ('E_', 'B_'):
        print(f'{family} voice power vs PCSX2 (rows): ' +
              ' | '.join(f"{n} {result[n]['families'][family]['power_db_vs_pcsx2']} dB ({result[n]['families'][family]['rows']})"
                         for n in names))
    (OUT / f'compare-{"-".join(names)}.json').write_text(json.dumps(result, indent=1), encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build-pcsx2')
    b.add_argument('--src', type=Path, required=True, help="prepare_pcsx2_compare.py's PCSX2 tree")
    for name in ('retail', 'port'):
        r = sub.add_parser(name)
        r.add_argument('label')
        r.add_argument('--track', type=int, default=15, help='Right presses on the track chooser (15 Mt. Liddellroch FR)')
        r.add_argument('--stop', type=int, default=5400, help='race clock after GO to stop at (60 a second)')
        r.add_argument('--timeout', type=float, default=3600)
        if name == 'retail':
            r.add_argument('--pcsx2', default=str(ROOT / 'build/port/compare-pcsx2-src/bin' / (EXE_NAME + '.exe')))
        else:
            r.add_argument('--exe', required=True)
    c = sub.add_parser('compare')
    c.add_argument('labels', nargs='+', help='the PCSX2 run first, then port runs')
    c.add_argument('--bank', default='AUBE_S.BNK', help='the level bank on the disc (SND/...)')
    for p in sub.choices.values():
        p.add_argument('--iso', default=os.environ.get('DHD_TEST_ISO', ''))
    a = ap.parse_args()
    if a.command != 'build-pcsx2' and not Path(a.iso).is_file():
        ap.error('set DHD_TEST_ISO or give --iso')
    if a.command == 'build-pcsx2':
        build_pcsx2(a.src.resolve())
    elif a.command == 'compare':
        compare_runs(a)
    else:
        run(a.command, a)


if __name__ == '__main__':
    main()
